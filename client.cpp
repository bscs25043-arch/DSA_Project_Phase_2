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

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
}
bool moveBy(DebugSession &s, int32_t delta)
{
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
}
bool cmdNext(DebugSession &s) {}
bool cmdPrv(DebugSession &s) {}
bool cmdJmpNext(DebugSession &s, int32_t n) {}
bool cmdJmpPrv(DebugSession &s, int32_t n) { }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
}
void printHelp()
{
    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
}
//int main(int argc, char *argv[])
//{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

//}
int main(int argc, char *argv[])
{
    const char* testFile = (argc > 1) ? argv[1] : "session.tdbg";
    std::cout << "=== Testing Client Offline .tdbg Reader ===" << std::endl;

    TdbgReader reader;
    if (!openTdbg(testFile, reader))
    {
        std::cerr << "[Client Test Error] Could not open or parse " << testFile << std::endl;
        return 1;
    }

    std::cout << "[Client Test] Magic: " << string(reader.header.magic, 4)
              << " | Version: " << reader.header.version
              << " | Step Count: " << reader.header.stepCount << std::endl;

    // Test reading step 0
    if (reader.header.stepCount > 0)
    {
        Snapshot* s = readSnapshotAt(reader, 0);
        if (s)
        {
            std::cout << "[Client Test] Successfully read Step 0 Snapshot:" << std::endl;
            printSnapshot(0, reader.header.stepCount, *s);
            delete s;
        }
    }

    closeTdbg(reader);
    return 0;
}