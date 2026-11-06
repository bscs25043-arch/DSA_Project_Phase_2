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

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// Stack: back the live Call Stack during execution
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
    // Implement these functions:
    Stack()
    {
        top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {
        if (count == MAX_STACK_DEPTH)
            throw std::out_of_range("Max Limit Reached for Stack!!!\n");
        Node* Next_Node = new Node;
        Next_Node->data = val;
        Next_Node->next = top;
        top = Next_Node;
        count = count + 1;
    }
    T pop()
    {
        if (count == 0)
        {
            throw std::out_of_range("Max_Limit_Reached_For_Stack_Depth\n");
        }
        T deleted = top->data;
        Node* tem = top->next;
        delete top;
        top = tem;
        count = count - 1;
        return deleted;
    
    }
    T& peek()
    {
        if (count == 0)
        {
            throw std::out_of_range("Max Limit Reached for Stack!!!\n");
        }
        return top->data;
       
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
   int32_t snapshot_into(T out[], int32_t maxLen)
{
    int32_t copied = 0;

    Node* curr = top;

    while (curr != nullptr && copied < maxLen)
    {
        out[copied] = curr->data;
        copied++;

        curr = curr->next;
    }

    return copied;
}
};

struct Snapshot;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};

class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        if (s == nullptr)
            throw std::invalid_argument("Cannot record null Snapshot");

        TimelineNode* temp = new TimelineNode();
        temp->data = s;
        temp->next = nullptr;
        temp->prev = tail;
       if (head == nullptr)
       {
        head = temp;
        head->prev = nullptr;
        tail = temp;
        tail->next = nullptr;
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
        if (head != nullptr)
            return head;
        else
        {
            throw std::out_of_range("Head is Null and ThereFore Cannot be accessed!\n");
        }
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

struct Variable
{
    std::string name;
    int32_t value;
};
struct Frame
{
    std::string func_name;
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
    char magic[4]; 
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
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

struct FuncEntry
{
    std::string funcName;
    int64_t byteOffsetInResolveBin; 
};
struct PendingPatch
{
    std::int64_t byteOffsetOfOffsetField; 
    std::string targetFuncName;
};

bool readSourceLine(std::ifstream& in, std::string& out)
{
    std::string line;
    while (std::getline(in, line))
    {
        size_t pos = line.find_first_not_of(" \t\r\n");

        if (pos != std::string::npos)
        {
            out = line.substr(pos);
            return true;
        }
    }
    return false;
}

std::string firstWord(const std::string& line)
{
    size_t spacePos = line.find_first_of(" \t");
    if (spacePos == std::string::npos)
    {
        return line;
    }
    else
    {
        return line.substr(0, spacePos);
    }
}
std::string secondWord(const std::string& line)
{
    size_t firstSpace = line.find_first_of(" \t");
    if (firstSpace == std::string::npos)
        return ("");
    size_t secondStart = line.find_first_not_of(" \t", firstSpace);
    if (secondStart == std::string::npos)
        return ("");
    size_t secondEnd = line.find_first_of(" \t", secondStart);

    if (secondEnd == std::string::npos)
    {
        return line.substr(secondStart);
    }
    else
    {
        return line.substr(secondStart, secondEnd - secondStart);
    }
}
bool validateProgram(const char* sourcePath)
{
    std::string line;
    std::ifstream file(sourcePath);
    if (!file.is_open())
        throw std::runtime_error("Failed to open source file in validateProgram");
    int groove = 0;
    while (readSourceLine(file, line))
    {
        std::string kw = firstWord(line);
        if (kw == "func")
        {
            groove = groove + 1;
            if (groove > 1)
                return false;
        }
        if (kw == "func_end")
        {
           groove--;

         if (groove < 0)
          return false;
        }
    }
    if (groove != 0)
        return false;
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const std::string& text)
{
    if (!f)
        throw std::runtime_error("Invalid file pointer in writeResolveRecord");

    int64_t current_pos = ftell(f);
    int32_t siz = static_cast<int32_t>(text.length());
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&siz, sizeof(int32_t),1, f);
    fwrite(text.c_str(), sizeof(char), siz, f);

    return current_pos;
}
int64_t readResolveRecord(FILE* f, std::string& outText)
{
    if (!f)
        throw std::runtime_error("Invalid file pointer in readResolveRecord");

    int64_t off_Field = 0;
    int32_t siz = 0;

    if (fread(&off_Field, sizeof(int64_t), 1, f) != 1)
        return -1;
    if (fread(&siz, sizeof(int32_t), 1, f) != 1)
        return -1;

    outText.resize(siz);
    if (fread(&outText[0], sizeof(char), siz, f) != static_cast<size_t>(siz)) return -1;

    return off_Field;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    std::ifstream in(sourcePath);

    if (!in.is_open())
        throw std::runtime_error("Failed to open sourcePath in resolveProgram");

    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;

    std::string line;
    int64_t curr_Offset = 0;

    while (readSourceLine(in, line))
    {
        if (firstWord(line) == "func")
        {
            if (funcCount >= MAX_FUNCS)
            {
                in.close();
                throw std::out_of_range("Max limit reached for function count in resolveProgram");
            }
            funcArray[funcCount].funcName = secondWord(line);
            funcArray[funcCount].byteOffsetInResolveBin = curr_Offset;
            funcCount++;
        }
        curr_Offset = curr_Offset + 8 + 4 + line.length();
    }
    in.close();

    FILE* out = fopen(resolveBinPath, "wb");

    if (out == nullptr)
        throw std::runtime_error("Failed to open resolveBinPath for writing");

    in.open(sourcePath);

    if (!in.is_open())
    {
        fclose(out);
        throw std::runtime_error("Failed to re-open sourcePath in resolveProgram");
    }

    curr_Offset = 0;

    while (readSourceLine(in, line))
    {
        std::string kw = firstWord(line);
        if (kw == "call")
        {
            std::string target = secondWord(line);
            int64_t targetOffset = -1;
            for (int i = 0; i < funcCount; i++)
            {
                if (funcArray[i].funcName == target)
                {
                    targetOffset = funcArray[i].byteOffsetInResolveBin;
                    break;
                }
            }
            if (targetOffset == -1)
            {
                in.close();
                fclose(out);
                throw std::runtime_error("Call to undefined function: " + target);
            }
            std::string patchedLine = "call " + std::to_string(targetOffset);
            size_t secondSpace = line.find(' ');

           if (secondSpace != std::string::npos)
           {
           secondSpace = line.find(' ', secondSpace + 1);

            if (secondSpace != std::string::npos)
            {
             patchedLine += line.substr(secondSpace);
             }
}

           writeResolveRecord(out, targetOffset, patchedLine);
        }
        else
        {
            writeResolveRecord(out, curr_Offset, line);
        }

        curr_Offset = curr_Offset + 8 + 4 + line.length();
    }
    in.close();
    fclose(out);

    for (int i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
            return funcArray[i].byteOffsetInResolveBin;
    }
    throw std::runtime_error("Main function not found in source program");
}

// PASS 0x2: EXECUTION (tokenization happens here)
std::string getFunctionNameAtOffset(FILE* f, int64_t offset)
{
    fseek(f, offset, SEEK_SET);

    std::string line;

    readResolveRecord(f, line);

    return secondWord(line);
}
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    std::string text;
};
int32_t tokenizeLine(const std::string& line, Token tokens[], int32_t maxTokens)
{
    if (maxTokens < 2)
        throw std::invalid_argument("maxTokens must be at least 2");

    std::string Fw = firstWord(line);
    tokens[0].text = Fw;
    tokens[0].type = KEYWORD;
    std::string Sw = secondWord(line);
    tokens[1].text = Sw;
    tokens[1].type = IDENTIFIER;

    int counter = 2;
    int dex = line.find(Sw);
    if (dex == std::string::npos)
        return counter;
    dex = dex + Sw.length();
    std::string word;

    while (dex < line.length())
    {
        if (line[dex] != '\n' && line[dex] != '\t' && line[dex] != ' ')
        {
            word = word + line[dex];
        }
        else
        {
            if (!word.empty() && counter < maxTokens)
            {
                tokens[counter].text = word;
                tokens[counter].type = PARAM;
                counter = counter + 1;
                word.erase();
            }
        }
        dex = dex + 1;
    }

    if (!word.empty() && counter < maxTokens)
    {
        tokens[counter].text = word;
        tokens[counter].type = PARAM;
        counter = counter + 1;
    }
    return counter;
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
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* f = fopen(resolveBinPath, "rb");
    if (!f)
        throw std::runtime_error("Failed to open resolveBinPath in executeProgram");

    Stack<Frame> staq;

    Frame Fr;
    Fr.func_name = "main";
    Fr.argc = 0;
    Fr.localCount = 0;
    Fr.returnLine = -1;
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

            if (kw == "set")
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
                    for (int i = 0; i < curr_Fr.localCount; i++)
                    {
                        if (curr_Fr.locals[i].name == valStr)
                            val = curr_Fr.locals[i].value;
                    }
                    for (int i = 0; i < curr_Fr.argc; i++)
                    {
                        if (curr_Fr.argv[i].name == valStr)
                            val = curr_Fr.argv[i].value;
                    }
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

                int32_t v1 = 0;
                int32_t v2 = 0;

                for (int i = 0; i < curr_Fr.localCount; i++)
                {
                    if (curr_Fr.locals[i].name == get_var)
                        v1 = curr_Fr.locals[i].value;
                }
                for (int i = 0; i < curr_Fr.argc; i++)
                {
                    if (curr_Fr.argv[i].name == get_var)
                        v1 = curr_Fr.argv[i].value;
                }

                if (isdigit(valStr[0]) || (valStr[0] == '-' && valStr.length() > 1))
                {
                    v2 = std::stoi(valStr);
                }
                else
                {
                    for (int i = 0; i < curr_Fr.localCount; i++)
                    {
                        if (curr_Fr.locals[i].name == valStr)
                            v2 = curr_Fr.locals[i].value;
                    }
                    for (int i = 0; i < curr_Fr.argc; i++)
                    {
                        if (curr_Fr.argv[i].name == valStr)
                            v2 = curr_Fr.argv[i].value;
                    }
                }

                if (kw == "add") v1 = v1 + v2;
                else if (kw == "sub") v1 = v1 - v2;
                else if (kw == "mul") v1 = v1 * v2;
                else if (kw == "div")
                {
                    if (v2 == 0)
                    {
                        fclose(f);
                        throw std::runtime_error("Division by zero error");
                    }
                    v1 = v1 / v2;
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
                else if (!updated)
                {
                    fclose(f);
                    throw std::out_of_range("Max limit reached for local variables in Frame");
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

                    for (int j = 0; j < curr_Fr.localCount; j++)
                    {
                        if (curr_Fr.locals[j].name == argName)
                            argVal = curr_Fr.locals[j].value;
                    }
                    for (int j = 0; j < curr_Fr.argc; j++)
                    {
                        if (curr_Fr.argv[j].name == argName)
                            argVal = curr_Fr.argv[j].value;
                    }

                    New_Fr.argv[New_Fr.argc].name = argName;
                    New_Fr.argv[New_Fr.argc].value = argVal;
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
// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
   
    FILE* f = fopen(tdbgPath, "wb");
    if (!f)
        throw std::runtime_error("Failed to open tdbgPath for writing in writeTdbg");

    int32_t tot_step = timeline.getStepCount();

   
    TTDBHeader header;
    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';
    header.version = 1;
    header.stepCount = tot_step;
    header.indexOffset = 0;
    writeHeader(f, header);

    int64_t* int_dexs = new int64_t[tot_step > 0 ? tot_step : 1];
    TimelineNode* curr = nullptr;

    if (tot_step > 0)
    {
        curr = timeline.begin();
    }

    int32_t dex = 0;
    while (curr != nullptr && dex < tot_step)
    {
        int_dexs[dex] = ftell(f);

        if (curr->data != nullptr)
        {
            fwrite(curr->data, sizeof(Snapshot), 1, f);
        }

        dex = dex+1;
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

// main section

int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("Source.bin", "Resolve.bin");

    Timeline timeline;
    executeProgram("Resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "Session.tdbg");

    return 0;
}