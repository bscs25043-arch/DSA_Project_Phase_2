#pragma once
#include "iostream"
#include "string"
#include <netinet/in.h>
#include <arpa/inet.h>
// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
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

inline uint32_t computeCRC32(const char* data, size_t length, uint32_t crc = 0xFFFFFFFF)
{
    for (size_t i = 0; i < length; i++)
    {
        crc ^= static_cast<uint8_t>(data[i]);
        for (int k = 0; k < 8; k++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320U;
            else
                crc = crc >> 1;
        }
    }
    return ~crc;
}
