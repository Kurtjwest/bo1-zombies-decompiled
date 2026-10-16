#include "db_stream_load.h"
#include "db_stream.h"
#include <universal/assertive.h>
#include <string.h>
#include "db_file_load.h"
#include <clientscript/cscr_stringlist.h>

StreamDelayInfo g_streamDelayArray[4096];

void __cdecl Load_Stream(bool atStreamStart, unsigned __int8 *ptr, int size)
{
    if (atStreamStart != (ptr == DB_GetStreamPos())
        && !Assert_MyHandler(
            "C:\\projects_pc\\cod\\codsrc\\src\\database\\db_stream_load.cpp",
            21,
            0,
            "%s",
            "atStreamStart == (ptr == DB_GetStreamPos())"))
    {
        __debugbreak();
    }
    if (atStreamStart && size)
    {
        if (g_streamPosIndex - 1 < 3)
        {
            if (g_streamPosIndex == 1)
            {
                memset(ptr, 0, size);
            }
            else
            {
                if (g_streamDelayIndex >= 0x1000
                    && !Assert_MyHandler(
                        "C:\\projects_pc\\cod\\codsrc\\src\\database\\db_stream_load.cpp",
                        36,
                        0,
                        "g_streamDelayIndex doesn't index ARRAY_COUNT( g_streamDelayArray )\n\t%i not in [0, %i)",
                        g_streamDelayIndex,
                        4096))
                {
                    __debugbreak();
                }
                g_streamDelayArray[g_streamDelayIndex].ptr = ptr;
                g_streamDelayArray[g_streamDelayIndex++].size = size;
            }
        }
        else
        {
            DB_LoadXFileData(ptr, size);
        }
        DB_IncStreamPos(size);
    }
}

void __cdecl Load_DelayStream()
{
    unsigned int index; // [esp+4h] [ebp-8h]

    for (index = 0; index < g_streamDelayIndex; ++index)
        DB_LoadXFileData((unsigned __int8 *)g_streamDelayArray[index].ptr, g_streamDelayArray[index].size);
}

void __cdecl DB_ConvertOffsetToAlias(unsigned int *data)
{
    unsigned int offset; // [esp+0h] [ebp-8h]

    offset = *data;
    if ( (!*data || offset >= 0xFFFFFFFE)
        && !Assert_MyHandler(
                    "C:\\projects_pc\\cod\\codsrc\\src\\database\\db_stream_load.cpp",
                    67,
                    0,
                    "%s\n\t(offset) = %i",
                    "(offset && (offset != 0xFFFFFFFF) && (offset != 0xFFFFFFFE))",
                    offset) )
    {
        __debugbreak();
    }
    *data = *(unsigned int *)&g_streamBlocks[(offset - 1) >> 29].data[(offset - 1) & 0x1FFFFFFF];
}

// *data is a 32-bit pointer field holding a fastfile block offset; it becomes a Ptr32 handle to that address.
void __cdecl DB_ConvertOffsetToPointer(unsigned int *data)
{
    *data = Ptr32_Encode(&g_streamBlocks[(unsigned int)(*data - 1) >> 29].data[(*data - 1) & 0x1FFFFFFF]);
}

void __cdecl Load_XStringCustom(Ptr32<const char> *str)
{
    unsigned __int8 *numBytesLoaded; // [esp+4h] [ebp-8h]

    numBytesLoaded = DB_LoadXFileDataNullTerminated((unsigned __int8 *)*str);
    DB_IncStreamPos((int)(intptr_t)numBytesLoaded);
}

void __cdecl Load_TempStringCustom(Ptr32<const char> *str)
{
    const char *String; // [esp+0h] [ebp-4h]

    Load_XStringCustom(str);
    String = *str;
    // the zone's script string list keeps each string's SL id in its pointer slot (Load_ScriptStringCustom reads it
    // back as an integer): store the id itself, not a pointer encoding of it
    Ptr32_Raw(*str) = String ? (uint32_t)SL_GetString((char *)String, 4u, SCRIPTINSTANCE_SERVER) : 0u;
}

