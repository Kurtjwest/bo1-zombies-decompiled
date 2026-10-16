#pragma once
#include <universal/ptr32.h>

struct GenericAabbTree // sizeof=0x10
{
    int firstItem;
    int itemCount;
    int firstChild;
    int childCount;
};

struct GenericAabbTreeOptions // sizeof=0x28
{                                       // XREF: R_GenerateHighmipAabbs/r
                                        // XModelReadSurface_BuildCollisionTree/r
    Ptr32<void> items;                        // XREF: R_GenerateHighmipAabbs+2C/w
                                        // R_GenerateHighmipAabbs+141/w ...
    int itemCount;                      // XREF: R_GenerateHighmipAabbs+2F/w
                                        // R_GenerateHighmipAabbs+147/w ...
    int itemSize;                       // XREF: R_GenerateHighmipAabbs+32/w
                                        // R_GenerateHighmipAabbs+14A/w ...
    int maintainValidBounds;            // XREF: R_GenerateHighmipAabbs+35/w
                                        // R_GenerateHighmipAabbs+4A/w ...
    Ptr32<float[3]> mins;                   // XREF: R_GenerateHighmipAabbs+38/w
                                        // R_GenerateHighmipAabbs+85/w ...
    Ptr32<float[3]> maxs;                   // XREF: R_GenerateHighmipAabbs+3B/w
                                        // R_GenerateHighmipAabbs+BC/w ...
    Ptr32<GenericAabbTree> treeNodePool;      // XREF: R_GenerateHighmipAabbs+3E/w
                                        // R_GenerateHighmipAabbs+D2/w ...
    int treeNodeLimit;                  // XREF: R_GenerateHighmipAabbs+41/w
                                        // R_GenerateHighmipAabbs+D5/w ...
    int minItemsPerLeaf;                // XREF: R_GenerateHighmipAabbs+44/w
                                        // R_GenerateHighmipAabbs+DC/w ...
    int maxItemsPerLeaf;                // XREF: R_GenerateHighmipAabbs+47/w
                                        // R_GenerateHighmipAabbs+E3/w ...
};


int __cdecl BuildAabbTree(const GenericAabbTreeOptions *options);
void __cdecl BuildAabbTree_r(GenericAabbTree *tree, const GenericAabbTreeOptions *options, int *remap);
int __cdecl SplitAabbTree(int count, const GenericAabbTreeOptions *options, int *remap, int *midStart, int *lastStart);
bool __cdecl PickAabbSplitPlane(
                float (*mins)[3],
                float (*maxs)[3],
                int *remap,
                int count,
                int *chosenAxis,
                float *chosenDist);
int __cdecl compare_floats(float *e0, float *e1);
double __cdecl AddedVolume(const float *addedmins, const float *addedmaxs, const float *mins, const float *maxs);
void __cdecl CreateAabbSubTrees(
                GenericAabbTree *tree,
                const GenericAabbTreeOptions *options,
                int *remap,
                int firstIndex,
                int count);
GenericAabbTree *__cdecl AllocAabbTreeNode(const GenericAabbTreeOptions *options);
