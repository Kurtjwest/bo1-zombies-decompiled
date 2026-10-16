#pragma once
#include <universal/ptr32.h>

struct nodetype // sizeof=0x14
{                                       // XREF: huff_t/r node_t/r
    Ptr32<nodetype> left;
    Ptr32<nodetype> right;
    Ptr32<nodetype> parent;
    int weight;
    int symbol;
};

struct huff_t // sizeof=0x4C14
{                                       // XREF: huffman_t/r
    int blocNode;
    int blocPtrs;
    Ptr32<nodetype> tree;                     // XREF: MSG_ReadBitsCompress(uchar const *,int,uchar *,int)+95/r
    Ptr32<nodetype> loc[257];
    Ptr32<Ptr32<nodetype>> freelist;
    nodetype nodeList[768];
    Ptr32<nodetype> nodePtrs[768];
};

struct huffman_t // sizeof=0x4C14
{                                       // XREF: .data:huffman_t msgHuff/r
    huff_t compressDecompress;          // XREF: MSG_ReadBitsCompress(uchar const *,int,uchar *,int)+95/r
};

int __cdecl get_bit(const unsigned __int8 *fin);
void __cdecl Huff_offsetReceive(nodetype *node, int *ch, const unsigned __int8 *fin, int *offset);
void __cdecl huffman_send(nodetype *node, nodetype *child, unsigned __int8 *fout);
void __cdecl add_bit(char bit, unsigned __int8 *fout);
void __cdecl Huff_offsetTransmit(huff_t *huff, int ch, unsigned __int8 *fout, int *offset);
void __cdecl Huff_Init(huffman_t *huff);
nodetype *__cdecl Huff_initNode(huff_t *huff, int ch, int weight);
int __cdecl nodeCmp(const void *left, const void *right);
void __cdecl Huff_BuildFromData(huff_t *huff, const int *msg_hData);
