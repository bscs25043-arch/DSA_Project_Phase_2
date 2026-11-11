// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.cpp"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader 
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    FILE* file; 
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock,char* buffer,int totalBytes)
{
    int recved = 0;
    while (recved < totalBytes)
    {
        int rez = recv( sock,buffer + recved,totalBytes - recved,0);

        if (rez <= 0)
        {
            return false;
        }

        recved = recved +rez;
    }

    return true;
}
bool sendAll(int sock,const char* buffer,int totalBytes)
{
    int sent = 0;

    while (sent < totalBytes)
    {
        int rez = send(sock,buffer + sent,totalBytes - sent,0);
        if (rez <= 0)
        {
            return false;
        }
        sent = sent+rez;
    }

    return true;
}

bool openTdbg(const char *path, TdbgReader &r)
{
    r.file = fopen(path, "rb");
    if (!r.file) return false;

    if (fread(r.header.magic, 1, 4, r.file) != 4 ||
        fread(&r.header.version, sizeof(int32_t), 1, r.file) != 1 ||
        fread(&r.header.stepCount, sizeof(int32_t), 1, r.file) != 1 ||
        fread(&r.header.indexOffset, sizeof(int64_t), 1, r.file) != 1)
    {
        fclose(r.file);
        return false;
    }

    if (memcmp(r.header.magic, "TTDB", 4) != 0)
    {
        fclose(r.file);
        return false;
    }

    r.index.resize(r.header.stepCount);
    fseek(r.file, r.header.indexOffset, SEEK_SET);
    if (r.header.stepCount > 0 && fread(r.index.data(), sizeof(int64_t), r.header.stepCount, r.file) != static_cast<size_t>(r.header.stepCount))
    {
        fclose(r.file);
        return false;
    }
    return true;
}

void closeTdbg(TdbgReader &r)
{
    if (r.file)
    {
        fclose(r.file);
        r.file = nullptr;
    }
    r.index.clear();
}
string readBinaryString(FILE* f)
{
    int32_t len = 0;
    if (fread(&len, sizeof(int32_t), 1, f) != 1) return "";
    string s(len, '\0');
    if (len > 0) fread(&s[0], 1, len, f);
    return s;
}

Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (step < 0 || step >= r.header.stepCount || !r.file) return nullptr;
    
    fseek(r.file, r.index[step], SEEK_SET);

    Snapshot* s = new Snapshot();
    if (fread(&s->stackDepth, sizeof(int32_t), 1, r.file) != 1)
    {
        delete s;
        return nullptr;
    }

    for (int i = 0; i < s->stackDepth; i++)
    {
        Frame& fr = s->callStack[i];
        fr.func_name = readBinaryString(r.file);

        if (fread(&fr.argc, sizeof(int32_t), 1, r.file) != 1) { delete s; return nullptr; }
        for (int a = 0; a < fr.argc; a++)
        {
            fr.argv[a].name = readBinaryString(r.file);
            fread(&fr.argv[a].value, sizeof(int32_t), 1, r.file);
        }

        fread(&fr.returnLine, sizeof(int32_t), 1, r.file);

        if (fread(&fr.localCount, sizeof(int32_t), 1, r.file) != 1) { delete s; return nullptr; }
        for (int l = 0; l < fr.localCount; l++)
        {
            fr.locals[l].name = readBinaryString(r.file);
            fread(&fr.locals[l].value, sizeof(int32_t), 1, r.file);
        }
    }
    return s;
}


// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================
int connectToHost(const char* host, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0 || connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}

bool uploadSource(int sock, const char *sourcePath)
{
    FILE* f = fopen(sourcePath, "rb");
    if (!f) return false;
    char buffer[IO_BUFFER_SIZE];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        if (!sendAll(sock, buffer, static_cast<int>(n)))
        {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    shutdown(sock, SHUT_WR);
    return true;
}

bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    vector<char> rawData;
    char buf[IO_BUFFER_SIZE];
    ssize_t res;
    while ((res = recv(sock, buf, sizeof(buf), 0)) > 0)
    {
        rawData.insert(rawData.end(), buf, buf + res);
    }
    if (rawData.size() < 5)
    {
        outError = "invalid response length from server";
        return false;
    }

    uint8_t status = rawData[0];
    size_t payloadLen = rawData.size() - 5;
    uint32_t recCrc;
    memcpy(&recCrc, &rawData[rawData.size() - 4], 4);

    uint32_t calcCrc = computeCRC32(rawData.data(), rawData.size() - 4);
    if (calcCrc != recCrc)
    {
        outError = "checksum mismatch, response corrupted";
        return false;
    }

    if (status != 0)
    {
        outError = string(rawData.data() + 1, payloadLen);
        return false;
    }

    FILE* f = fopen(outTdbgPath, "wb");
    if (!f)
    {
        outError = "failed to save output .tdbg file";
        return false;
    }
    fwrite(rawData.data() + 1, 1, payloadLen, f);
    fclose(f);
    return true;
}

void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.reader.file = nullptr;
    s.currentStep = 0;
}

void closeSession(DebugSession &s)
{
    if (s.active_flag)
    {
        closeTdbg(s.reader);
        s.active_flag = 0;
    }
}
// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "=== Step [" << (step + 1) << "/" << totalSteps << "] ===" << endl;
    cout << "Call Stack Depth: " << s.stackDepth << endl;
    for (int i = s.stackDepth - 1; i >= 0; i--)
    {
        const Frame& fr = s.callStack[i];
        cout << "  Frame [" << i << "]: " << fr.func_name << "()";
        if (fr.argc > 0)
        {
            cout << " Args: (";
            for (int a = 0; a < fr.argc; a++)
            {
                cout << fr.argv[a].name << "=" << fr.argv[a].value << (a + 1 < fr.argc ? ", " : "");
            }
            cout << ")";
        }
        cout << endl;

        if (fr.localCount > 0)
        {
            cout << "    Locals: ";
            for (int l = 0; l < fr.localCount; l++)
            {
                cout << fr.locals[l].name << "=" << fr.locals[l].value << (l + 1 < fr.localCount ? ", " : "");
            }
            cout << endl;
        }
    }
}

bool showCurrent(DebugSession &s)
{
    if (!s.active_flag)
    {
        cout << "No active debug session. Run 'start <file>' first." << endl;
        return false;
    }
    Snapshot* snap = readSnapshotAt(s.reader, static_cast<int32_t>(s.currentStep));
    if (!snap)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot(static_cast<int32_t>(s.currentStep), s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}

bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "No active session." << endl;
        return false;
    }
    int64_t nextStep = s.currentStep + delta;
    if (nextStep < 0 || nextStep >= s.reader.header.stepCount)
    {
        cout << "Step out of bounds [1.." << s.reader.header.stepCount << "]" << endl;
        return false;
    }
    s.currentStep = nextStep;
    return showCurrent(s);
}

bool cmdStart(DebugSession &s, const string &sourcePath)
{
    int sock = connectToHost(LB_HOST.c_str(), LB_PORT);
    if (sock < 0)
    {
        cout << "Error: Could not connect to load balancer at " << LB_HOST << ":" << LB_PORT << endl;
        return false;
    }
    if (!uploadSource(sock, sourcePath.c_str()))
    {
        cout << "Error sending source file" << endl;
        close(sock);
        return false;
    }
    string err;
    if (!receiveResponse(sock, TDBG_FILE, err))
    {
        cout << "Error: " << err << endl;
        close(sock);
        return false;
    }
    close(sock);

    TdbgReader newReader;
    if (!openTdbg(TDBG_FILE, newReader))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }

    if (s.active_flag) closeTdbg(s.reader);

    s.reader = newReader;
    s.active_flag = 1;
    s.currentStep = 0;
    return showCurrent(s);
}

bool cmdNext(DebugSession &s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession &s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession &s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession &s, int32_t n) { return moveBy(s, -n); }

void printHelp()
{
    cout << "Commands:" << endl;
    cout << "  start <file.bin>    - Compile & debug trace file" << endl;
    cout << "  next                - Step forward" << endl;
    cout << "  prv                 - Step backward" << endl;
    cout << "  jmp next <n>        - Jump n steps forward" << endl;
    cout << "  jmp prv <n>         - Jump n steps backward" << endl;
    cout << "  help                - Show this help message" << endl;
    cout << "  quit / exit         - Exit debugger" << endl;
}
// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    size_t i = 0;
    size_t len = line.length();
    vector<string> words;

    while (i < len)
    {
        while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) i++;
        if (i >= len) break;

        size_t start = i;
        while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') i++;

        words.push_back(line.substr(start, i - start));
    }

    if (words.empty()) return true;
    if (words[0] == "ttdb") words.erase(words.begin());
    if (words.empty()) return true;

    string cmd = words[0];
    if (cmd == "start")
    {
        if (words.size() < 2) { cout << "Usage: start <file.bin>" << endl; return false; }
        return cmdStart(s, words[1]);
    }
    else if (cmd == "next") { return cmdNext(s); }
    else if (cmd == "prv") { return cmdPrv(s); }
    else if (cmd == "jmp")
    {
        if (words.size() < 3) { cout << "Usage: jmp next|prv <n>" << endl; return false; }
        int32_t n = atoi(words[2].c_str());
        if (words[1] == "next") return cmdJmpNext(s, n);
        if (words[1] == "prv") return cmdJmpPrv(s, n);
    }
    else if (cmd == "help") { printHelp(); return true; }
    else if (cmd == "quit" || cmd == "exit") { quitRequested = true; return true; }

    cout << "unknown command" << endl;
    printHelp();
    return false;
}
int main(int argc, char *argv[])
{
    if (argc > 1) 
    {
        LB_HOST = argv[1];
    }
    if (argc > 2) 
    {
        LB_PORT = atoi(argv[2]);
    }

    DebugSession session;
    initSession(session);

    cout << "   Time-Travel Debugger (TTDB) 2.0 CLI    " << endl;
    cout << "Target Load Balancer: " << LB_HOST << ":" << LB_PORT << endl << endl;
    printHelp();
    cout << endl;

    bool quitRequested = false;
    string line;

    while (!quitRequested)
    {
        cout << PROMPT;
        if (!getline(cin, line)) 
        {
            break; 
        }
        
        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    cout << "Da TTDB Debugger By Tehseen Ahmad (BSCS25043). Goodbye!" << endl;

    return 0;
}