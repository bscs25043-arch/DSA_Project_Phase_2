// ====================== = TIME - TRAVEL DEBUGGER - SERVER TEMPLATE ====================== =
// ====================== = TIME - TRAVEL DEBUGGER - SERVER TEMPLATE ====================== =

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.cpp"

// ---- Constants ----
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int32_t IO_BUFFER_SIZE = 64 * 1024;

template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    Stack() : top(nullptr), count(0) {}

    ~Stack()
    {
        while (!isEmpty()) pop();
    }

    void push(const T& val)
    {
        if (count == MAX_STACK_DEPTH)
            throw std::out_of_range("Max Limit Reached for Stack!!!");
        Node* Next_Node = new Node{val, top};
        top = Next_Node;
        count++;
    }

    T pop()
    {
        if (count == 0)
            throw std::out_of_range("Stack Underflow");
        T deleted = top->data;
        Node* tem = top->next;
        delete top;
        top = tem;
        count--;
        return deleted;
    }

    T& peek()
    {
        if (count == 0)
            throw std::out_of_range("Stack is empty");
        return top->data;
    }

    bool isEmpty() { return count == 0; }
    int32_t depth() { return count; }

    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t copied = 0;
        std::vector<T> temp;
        Node* curr = top;

        while (curr != nullptr && (int)temp.size() < maxLen)
        {
            temp.push_back(curr->data);
            curr = curr->next;
        }

        // Store bottom-to-top so index 0 is main
        for (int i = (int)temp.size() - 1; i >= 0; i--)
        {
            out[copied++] = temp[i];
        }

        return copied;
    }
};

struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};

class Timeline
{
    TimelineNode* head;
    TimelineNode* tail;
    int32_t stepCount;

public:
    Timeline() : head(nullptr), tail(nullptr), stepCount(0) {}

    ~Timeline()
    {
        TimelineNode* curr = head;
        while (curr)
        {
            TimelineNode* nxt = curr->next;
            delete curr->data;
            delete curr;
            curr = nxt;
        }
    }

    void record(Snapshot* s)
    {
        if (s == nullptr)
            throw std::invalid_argument("Cannot record null Snapshot");

        TimelineNode* temp = new TimelineNode{s, nullptr, tail};
        if (head == nullptr)
        {
            head = tail = temp;
        }
        else
        {
            tail->next = temp;
            tail = temp;
        }
        stepCount++;
    }

    TimelineNode* begin()
    {
        if (head == nullptr)
            throw std::out_of_range("Timeline is empty");
        return head;
    }

    int32_t getStepCount() { return stepCount; }
};

void writeHeader(FILE* f, const TTDBHeader& h)
{
    if (!f)
        throw std::runtime_error("Invalid file pointer in writeHeader");

    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

bool readSourceLine(std::ifstream& in, std::string& out)
{
    std::string line;
    while (std::getline(in, line))
    {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start != std::string::npos)
        {
            size_t end = line.find_last_not_of(" \t\r\n");
            out = line.substr(start, end - start + 1);
            return true;
        }
    }
    return false;
}

std::string firstWord(const std::string& line)
{
    size_t spacePos = line.find_first_of(" \t");
    return (spacePos == std::string::npos) ? line : line.substr(0, spacePos);
}

std::string secondWord(const std::string& line)
{
    size_t firstSpace = line.find_first_of(" \t");
    if (firstSpace == std::string::npos) return "";
    size_t secondStart = line.find_first_not_of(" \t", firstSpace);
    if (secondStart == std::string::npos) return "";
    size_t secondEnd = line.find_first_of(" \t", secondStart);
    return (secondEnd == std::string::npos) ? line.substr(secondStart) : line.substr(secondStart, secondEnd - secondStart);
}

bool validateProgram(const char* sourcePath)
{
    std::string line;
    std::ifstream file(sourcePath);
    if (!file.is_open())
        throw std::runtime_error("Failed to open source file in validateProgram");
    int groove = 0;
    std::vector<std::string> funcs;
    while (readSourceLine(file, line))
    {
        std::string kw = firstWord(line);
        if (kw == "func")
        {
            std::string fn = secondWord(line);
            for (const auto& f : funcs)
            {
                if (f == fn) return false;
            }
            funcs.push_back(fn);
            groove++;
            if (groove > 1) return false;
        }
        else if (kw == "func_end")
        {
            groove--;
            if (groove < 0) return false;
        }
    }
    return (groove == 0);
}

int64_t writeResolveRecord(FILE* f, int64_t offsetField, const std::string& text)
{
    if (!f) throw std::runtime_error("Invalid file pointer in writeResolveRecord");
    int64_t current_pos = ftell(f);
    int32_t siz = static_cast<int32_t>(text.length());
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&siz, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), sizeof(char), siz, f);
    return current_pos;
}

int64_t readResolveRecord(FILE* f, std::string& outText)
{
    if (!f) throw std::runtime_error("Invalid file pointer in readResolveRecord");
    int64_t off_Field = 0;
    int32_t siz = 0;
    if (fread(&off_Field, sizeof(int64_t), 1, f) != 1) return -1;
    if (fread(&siz, sizeof(int32_t), 1, f) != 1) return -1;
    outText.resize(siz);
    if (fread(&outText[0], sizeof(char), siz, f) != static_cast<size_t>(siz)) return -1;
    return off_Field;
}

int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    std::ifstream in(sourcePath);
    if (!in.is_open())
        throw std::runtime_error("Failed to open sourcePath in resolveProgram");

    std::vector<std::string> lines;
    std::string line;
    while (readSourceLine(in, line))
    {
        lines.push_back(line);
    }
    in.close();

    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;

    for (size_t i = 0; i < lines.size(); i++)
    {
        if (firstWord(lines[i]) == "func")
        {
            if (funcCount >= MAX_FUNCS)
                throw std::out_of_range("Max limit reached for function count in resolveProgram");
            funcArray[funcCount].funcName = secondWord(lines[i]);
            funcArray[funcCount].byteOffsetInResolveBin = 0;
            funcCount++;
        }
    }

    std::vector<int64_t> lineOffsets(lines.size(), 0);
    std::vector<std::string> patchedLines = lines;

    for (int iter = 0; iter < 2; iter++)
    {
        int64_t curr_Offset = 0;
        for (size_t i = 0; i < lines.size(); i++)
        {
            lineOffsets[i] = curr_Offset;
            std::string kw = firstWord(lines[i]);
            if (kw == "func")
            {
                std::string fn = secondWord(lines[i]);
                for (int f = 0; f < funcCount; f++)
                {
                    if (funcArray[f].funcName == fn)
                    {
                        funcArray[f].byteOffsetInResolveBin = curr_Offset;
                        break;
                    }
                }
            }
            if (kw == "call")
            {
                std::string target = secondWord(lines[i]);
                int64_t targetOffset = -1;
                for (int f = 0; f < funcCount; f++)
                {
                    if (funcArray[f].funcName == target)
                    {
                        targetOffset = funcArray[f].byteOffsetInResolveBin;
                        break;
                    }
                }
                if (targetOffset == -1 && iter == 1)
                {
                    throw std::runtime_error("Call to undefined function: " + target);
                }
                std::string patched = "call " + std::to_string(targetOffset == -1 ? 0 : targetOffset);
                size_t firstSpace = lines[i].find_first_of(" \t");
                if (firstSpace != std::string::npos)
                {
                    size_t secondStart = lines[i].find_first_not_of(" \t", firstSpace);
                    if (secondStart != std::string::npos)
                    {
                        size_t secondEnd = lines[i].find_first_of(" \t", secondStart);
                        if (secondEnd != std::string::npos)
                        {
                            patched += lines[i].substr(secondEnd);
                        }
                    }
                }
                patchedLines[i] = patched;
            }
            else
            {
                patchedLines[i] = lines[i];
            }

            curr_Offset += 8 + 4 + patchedLines[i].length();
        }
    }

    FILE* out = fopen(resolveBinPath, "wb");
    if (out == nullptr)
        throw std::runtime_error("Failed to open resolveBinPath for writing");

    for (size_t i = 0; i < lines.size(); i++)
    {
        writeResolveRecord(out, lineOffsets[i], patchedLines[i]);
    }
    fclose(out);

    for (int i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
            return funcArray[i].byteOffsetInResolveBin;
    }
    throw std::runtime_error("Main function not found in source program");
}

std::string getFunctionNameAtOffset(FILE* f, int64_t offset)
{
    fseek(f, offset, SEEK_SET);
    std::string line;
    readResolveRecord(f, line);
    return secondWord(line);
}

enum TokenType { KEYWORD, IDENTIFIER, PARAM };
struct Token { TokenType type; std::string text; };

int32_t tokenizeLine(const std::string& line, Token tokens[], int32_t maxTokens)
{
    int32_t count = 0;
    size_t i = 0;
    size_t len = line.length();

    while (i < len && count < maxTokens)
    {
        while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) i++;
        if (i >= len) break;

        size_t start = i;
        while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') i++;

        tokens[count].text = line.substr(start, i - start);
        if (count == 0) tokens[count].type = KEYWORD;
        else if (count == 1) tokens[count].type = IDENTIFIER;
        else tokens[count].type = PARAM;
        count++;
    }
    return count;
}

Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* temp = new Snapshot();
    temp->stackDepth = callStack.depth();
    if (!callStack.isEmpty())
    {
        callStack.snapshot_into(temp->callStack, MAX_STACK_DEPTH);
    }
    return temp;
}

int32_t getVariableValue(Frame& frame, const std::string& name)
{
    for (int i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name) return frame.locals[i].value;
    }
    for (int i = 0; i < frame.argc; i++)
    {
        if (frame.argv[i].name == name) return frame.argv[i].value;
    }
    throw std::runtime_error("Undefined variable: " + name);
}

void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* f = fopen(resolveBinPath, "rb");
    if (!f) throw std::runtime_error("Failed to open resolveBinPath in executeProgram");

    Stack<Frame> staq;
    Frame Fr{"main", 0, {}, -1, {}, 0, {}};
    staq.push(Fr);

    int64_t currentOffset = mainOffset;

    while (!staq.isEmpty())
    {
        fseek(f, currentOffset, SEEK_SET);
        std::string line;
        int64_t storedOffset = readResolveRecord(f, line);
        if (storedOffset == -1) break;

        int64_t nextOffset = currentOffset + 8 + 4 + line.length();
        Token tokens[MAX_TOKENS];
        int32_t counter = tokenizeLine(line, tokens, MAX_TOKENS);

        if (counter > 0)
        {
            std::string kw = tokens[0].text;
            Frame& curr_Fr = staq.peek();

            if (kw == "func")
            {
                for (int i = 2; i < counter && (i - 2) < curr_Fr.argc; i++)
                {
                    curr_Fr.argv[i - 2].name = tokens[i].text;
                }
                currentOffset = nextOffset;
            }
            else if (kw == "set")
            {
                std::string var_name = tokens[1].text;
                std::string valStr = tokens[2].text;
                int32_t val = 0;

                if (isdigit(valStr[0]) || (valStr[0] == '-' && valStr.length() > 1))
                {
                    val = std::stoi(valStr);
                }
                else
                {
                    val = getVariableValue(curr_Fr, valStr);
                }

                bool found = false;
                for (int i = 0; i < curr_Fr.localCount; i++)
                {
                    if (curr_Fr.locals[i].name == var_name)
                    {
                        curr_Fr.locals[i].value = val;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    for (int i = 0; i < curr_Fr.argc; i++)
                    {
                        if (curr_Fr.argv[i].name == var_name)
                        {
                            curr_Fr.argv[i].value = val;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found && curr_Fr.localCount < MAX_VARS_PER_FRAME)
                {
                    curr_Fr.locals[curr_Fr.localCount].name = var_name;
                    curr_Fr.locals[curr_Fr.localCount].value = val;
                    curr_Fr.localCount++;
                }
                else if (!found)
                {
                    fclose(f);
                    throw std::out_of_range("Max limit reached for local variables in Frame");
                }

                currentOffset = nextOffset;
            }
            else if (kw == "add" || kw == "sub" || kw == "mul" || kw == "div")
            {
                std::string get_var = tokens[1].text;
                std::string valStr = tokens[2].text;

                int32_t v1 = getVariableValue(curr_Fr, get_var);
                int32_t v2 = 0;

                if (isdigit(valStr[0]) || (valStr[0] == '-' && valStr.length() > 1))
                {
                    v2 = std::stoi(valStr);
                }
                else
                {
                    v2 = getVariableValue(curr_Fr, valStr);
                }

                if (kw == "add") v1 += v2;
                else if (kw == "sub") v1 -= v2;
                else if (kw == "mul") v1 *= v2;
                else if (kw == "div")
                {
                    if (v2 == 0)
                    {
                        fclose(f);
                        throw std::runtime_error("Division by zero error");
                    }
                    v1 /= v2;
                }

                bool updated = false;
                for (int i = 0; i < curr_Fr.localCount; i++)
                {
                    if (curr_Fr.locals[i].name == get_var)
                    {
                        curr_Fr.locals[i].value = v1;
                        updated = true;
                        break;
                    }
                }
                if (!updated)
                {
                    for (int i = 0; i < curr_Fr.argc; i++)
                    {
                        if (curr_Fr.argv[i].name == get_var)
                        {
                            curr_Fr.argv[i].value = v1;
                            updated = true;
                            break;
                        }
                    }
                }
                if (!updated && curr_Fr.localCount < MAX_VARS_PER_FRAME)
                {
                    curr_Fr.locals[curr_Fr.localCount].name = get_var;
                    curr_Fr.locals[curr_Fr.localCount].value = v1;
                    curr_Fr.localCount++;
                }

                currentOffset = nextOffset;
            }
            else if (kw == "call")
            {
                int64_t targetOffset = std::stoll(tokens[1].text);

                Frame New_Fr;
                New_Fr.func_name = getFunctionNameAtOffset(f, targetOffset);
                New_Fr.argc = 0;
                New_Fr.localCount = 0;
                New_Fr.returnLine = nextOffset;

                for (int i = 2; i < counter && New_Fr.argc < MAX_VARS_PER_FRAME; i++)
                {
                    std::string argName = tokens[i].text;
                    int32_t argVal = 0;

                    if (isdigit(argName[0]) || (argName[0] == '-' && argName.length() > 1))
                    {
                        argVal = std::stoi(argName);
                    }
                    else
                    {
                        argVal = getVariableValue(curr_Fr, argName);
                    }

                    New_Fr.argv[New_Fr.argc].name = argName;
                    New_Fr.argv[New_Fr.argc].value = argVal;
                    New_Fr.callerArgNames[New_Fr.argc] = argName;
                    New_Fr.argc++;
                }

                staq.push(New_Fr);
                currentOffset = targetOffset;
            }
            else if (kw == "func_end")
            {
                Frame popped = staq.pop();
                if (staq.isEmpty())
                {
                    Snapshot* finalSnap = buildSnapshot(staq);
                    timeline.record(finalSnap);
                    break;
                }

                Frame& callerFrame = staq.peek();
                for (int i = 0; i < popped.argc; i++)
                {
                    std::string origVar = popped.callerArgNames[i];
                    for (int j = 0; j < callerFrame.localCount; j++)
                    {
                        if (callerFrame.locals[j].name == origVar)
                            callerFrame.locals[j].value = popped.argv[i].value;
                    }
                    for (int j = 0; j < callerFrame.argc; j++)
                    {
                        if (callerFrame.argv[j].name == origVar)
                            callerFrame.argv[j].value = popped.argv[i].value;
                    }
                }

                currentOffset = popped.returnLine;
            }
            else
            {
                currentOffset = nextOffset;
            }
        }

        Snapshot* snap = buildSnapshot(staq);
        timeline.record(snap);
    }

    fclose(f);
}

void writeString(FILE* f, const std::string& s)
{
    int32_t n = static_cast<int32_t>(s.length());
    fwrite(&n, sizeof(int32_t), 1, f);
    fwrite(s.c_str(), 1, n, f);
}

void writeVariable(FILE* f, const Variable& v)
{
    writeString(f, v.name);
    fwrite(&v.value, sizeof(int32_t), 1, f);
}

void writeSnapshot(FILE* f, const Snapshot& s)
{
    fwrite(&s.stackDepth, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame& fr = s.callStack[i];
        writeString(f, fr.func_name);

        fwrite(&fr.argc, sizeof(int32_t), 1, f);
        for (int32_t a = 0; a < fr.argc; a++)
            writeVariable(f, fr.argv[a]);

        fwrite(&fr.returnLine, sizeof(int32_t), 1, f);

        fwrite(&fr.localCount, sizeof(int32_t), 1, f);
        for (int32_t l = 0; l < fr.localCount; l++)
            writeVariable(f, fr.locals[l]);
    }
}

void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    FILE* f = fopen(tdbgPath, "wb");
    if (!f)
        throw std::runtime_error("Failed to open tdbgPath for writing in writeTdbg");

    int32_t tot_step = timeline.getStepCount();

    TTDBHeader header;
    header.magic[0] = 'T'; header.magic[1] = 'T'; header.magic[2] = 'D'; header.magic[3] = 'B';
    header.version = 1;
    header.stepCount = tot_step;
    header.indexOffset = 0;
    writeHeader(f, header);

    int64_t* int_dexs = new int64_t[tot_step > 0 ? tot_step : 1];
    TimelineNode* curr = (tot_step > 0) ? timeline.begin() : nullptr;

    int32_t dex = 0;
    while (curr != nullptr && dex < tot_step)
    {
        int_dexs[dex] = ftell(f);

        if (curr->data != nullptr)
        {
            writeSnapshot(f, *curr->data);
        }

        dex++;
        curr = curr->next;
    }

    int64_t indexOffset = ftell(f);
    if (tot_step > 0)
    {
        fwrite(int_dexs, sizeof(int64_t), tot_step, f);
    }
    header.indexOffset = indexOffset;
    fseek(f, 0, SEEK_SET);

    fwrite(header.magic, 1, 4, f);
    fwrite(&header.version, sizeof(int32_t), 1, f);
    fwrite(&header.stepCount, sizeof(int32_t), 1, f);
    fwrite(&header.indexOffset, sizeof(int64_t), 1, f);

    delete[] int_dexs;
    fclose(f);
}

void runSocketServer(int port)
{
    int listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock < 0) return;

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(listenSock, 1) < 0)
    {
        close(listenSock);
        return;
    }

    int clientSock = accept(listenSock, nullptr, nullptr);
    close(listenSock);
    if (clientSock < 0) return;

    FILE* src = fopen("source.bin", "wb");
    char buffer[IO_BUFFER_SIZE];
    ssize_t bytesRead;
    while ((bytesRead = recv(clientSock, buffer, sizeof(buffer), 0)) > 0)
    {
        fwrite(buffer, 1, bytesRead, src);
    }
    fclose(src);

    std::string errorMsg = "";
    bool success = false;

    if (!validateProgram("source.bin"))
    {
        errorMsg = "Validation failed: unmatched func/func_end or nested functions";
    }
    else
    {
        try
        {
            int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
            Timeline timeline;
            executeProgram("resolve.bin", mainOffset, timeline);
            writeTdbg(timeline, "session.tdbg");
            success = true;
        }
        catch (const std::exception& ex)
        {
            errorMsg = ex.what();
        }
    }

    std::vector<char> response;
    if (success)
    {
        response.push_back(0); // Status 0 = Success
        FILE* tdbg = fopen("session.tdbg", "rb");
        if (tdbg)
        {
            size_t n;
            while ((n = fread(buffer, 1, sizeof(buffer), tdbg)) > 0)
            {
                response.insert(response.end(), buffer, buffer + n);
            }
            fclose(tdbg);
        }
    }
    else
    {
        response.push_back(1); // Status 1 = Error
        response.insert(response.end(), errorMsg.begin(), errorMsg.end());
    }

    uint32_t crc = computeCRC32(response.data(), response.size());
    char crcBuf[4];
    memcpy(crcBuf, &crc, 4);
    response.insert(response.end(), crcBuf, crcBuf + 4);

    send(clientSock, response.data(), response.size(), 0);
    close(clientSock);
}

int32_t main(int argc, char* argv[])
{
    if (argc > 1 && isdigit(argv[1][0]))
    {
        runSocketServer(std::stoi(argv[1]));
        return 0;
    }

    const char* sourceFile = (argc > 1) ? argv[1] : "source.bin";
    if (!validateProgram(sourceFile)) return 1;

    int64_t mainOffset = resolveProgram(sourceFile, "resolve.bin");
    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);
    writeTdbg(timeline, "session.tdbg");
    return 0;
}