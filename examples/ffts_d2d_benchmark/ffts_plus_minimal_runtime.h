#ifndef H2DCOPY_FFTS_PLUS_MINIMAL_RUNTIME_H
#define H2DCOPY_FFTS_PLUS_MINIMAL_RUNTIME_H

#include <cstddef>
#include <cstdint>

#ifndef RTS_API
#define RTS_API
#endif

using rtError_t = int32_t;
using rtStream_t = void *;

static constexpr int32_t RT_ERROR_NONE = 0;
static constexpr uint32_t RT_FFTS_PLUS_CTX_DESC_ADDR_TYPE_HOST = 0x0U;
static constexpr uint16_t RT_CTX_SUCCESSOR_NUM = 26;

enum rtFftsPlusType_t : uint16_t {
    RT_FFTS_PLUS_TYPE_RES1 = 2,
    RT_FFTS_PLUS_TYPE_RES2 = 3,
    RT_FFTS_PLUS_TYPE = 4,
};

enum rtFftsPlusContextType_t : uint16_t {
    RT_CTX_TYPE_AICORE = 0x0000,
    RT_CTX_TYPE_AIV = 0x0001,
    RT_CTX_TYPE_NOTIFY_WAIT = 0x0003,
    RT_CTX_TYPE_NOTIFY_RECORD = 0x0004,
    RT_CTX_TYPE_WRITE_VALUE = 0x0005,
    RT_CTX_TYPE_MIX_AIC = 0x0006,
    RT_CTX_TYPE_MIX_AIV = 0x0007,
    RT_CTX_TYPE_SDMA = 0x0008,
    RT_CTX_TYPE_FLUSH_DATA = 0x0009,
    RT_CTX_TYPE_INVALIDATE_DATA = 0x000A,
    RT_CTX_TYPE_WRITEBACK_DATA = 0x000B,
    RT_CTX_TYPE_AICPU = 0x000C,
    RT_CTX_TYPE_COND_SWITCH = 0x010D,
    RT_CTX_TYPE_CASE_SWITCH = 0x020D,
    RT_CTX_TYPE_AT_START = 0x0300,
    RT_CTX_TYPE_AT_END = 0x0400,
    RT_CTX_TYPE_LABEL = 0x0500,
    RT_CTX_TYPE_PERSISTENT_CACHE = 0x0600,
    RT_CTX_TYPE_DSA = 0x0700,
    RT_CTX_TYPE_WRITE_VALUE_RDMA = 0x0805,
};

#pragma pack(push)
#pragma pack(1)

struct rtStarsSqeHeader_t {
    uint8_t type : 6;
    uint8_t l1Lock : 1;
    uint8_t l1Unlock : 1;
    uint8_t ie : 2;
    uint8_t preP : 2;
    uint8_t postP : 2;
    uint8_t wrCqe : 1;
    uint8_t reserved : 1;
    uint16_t blockDim;
    uint16_t rtStreamId;
    uint16_t taskId;
};

struct rtFftsPlusSqe_t {
    rtStarsSqeHeader_t sqeHeader;
    uint16_t fftsType : 3;
    uint16_t cmo : 1;
    uint16_t scheduleDfxFlag : 1;
    uint16_t reserved1 : 7;
    uint16_t wrrRatio : 4;
    uint16_t dsaSqId : 11;
    uint16_t reserved2 : 5;
    uint16_t sqeIndex;
    uint8_t kernelCredit;
    uint8_t subType;
    uint32_t stackPhyBaseL;
    uint32_t stackPhyBaseH;
    uint16_t totalContextNum;
    uint16_t readyContextNum;
    uint16_t preloadContextNum;
    uint16_t timeout;
    uint16_t reserved6;
    uint16_t prefetchOstNum : 5;
    uint16_t reserved9 : 3;
    uint16_t cmaintOstNum : 5;
    uint16_t reserved10 : 3;
    uint16_t aicPrefetchLower : 5;
    uint16_t reserved11 : 3;
    uint16_t aicPrefetchUpper : 5;
    uint16_t reserved12 : 3;
    uint16_t aivPrefetchLower : 5;
    uint16_t reserved13 : 3;
    uint16_t aivPrefetchUpper : 5;
    uint16_t reserved14 : 3;
    uint32_t contextAddressBaseL;
    uint32_t contextAddressBaseH : 17;
    uint32_t reserved15 : 15;
    uint32_t reserved16[4];
};

struct rtFftsPlusComCtx_t {
    uint16_t contextType;
    uint8_t successorNum;
    uint8_t rsv1 : 7;
    uint8_t aten : 1;
    uint8_t rsv2;
    uint8_t rsv3;
    uint8_t predCntInit;
    uint8_t predCnt;
    uint32_t rsv4;
    uint16_t successorList[RT_CTX_SUCCESSOR_NUM];
    uint32_t rsv5[2];
    uint16_t threadId;
    uint16_t threadDim;
    uint32_t res6[13];
};

struct rtFftsPlusSdmaCtx_t {
    uint16_t contextType;
    uint8_t successorNum;
    uint8_t res1 : 6;
    uint8_t dumpSwitch : 1;
    uint8_t aten : 1;
    uint8_t res2;
    uint8_t res3;
    uint8_t predCntInit;
    uint8_t predCnt;
    uint32_t res4;
    uint16_t successorList[RT_CTX_SUCCESSOR_NUM];
    uint8_t res5;
    uint8_t res6 : 7;
    uint8_t atm : 1;
    uint16_t res7;
    uint16_t pmg : 2;
    uint16_t ns : 1;
    uint16_t partId : 8;
    uint16_t res8 : 1;
    uint16_t qos : 4;
    uint16_t res9;
    uint16_t threadId;
    uint16_t threadDim;
    uint32_t sdmaSqeHeader;
    uint16_t sourceStreamId;
    uint16_t sourceSubstreamId;
    uint16_t destinationStreamId;
    uint16_t destinationSubstreamId;
    uint32_t sourceAddressBaseL;
    uint32_t sourceAddressBaseH;
    uint32_t sourceAddressOffset;
    uint32_t destinationAddressBaseL;
    uint32_t destinationAddressBaseH;
    uint32_t destinationAddressOffset;
    uint32_t nonTailDataLength;
    uint32_t tailDataLength;
    uint32_t res10[2];
};

struct rtFftsPlusDumpInfo_t {
    const void *loadDumpInfo;
    const void *unloadDumpInfo;
    uint32_t loadDumpInfolen;
    uint32_t unloadDumpInfolen;
};

struct rtFftsPlusTaskInfo_t {
    const rtFftsPlusSqe_t *fftsPlusSqe;
    const void *descBuf;
    size_t descBufLen;
    rtFftsPlusDumpInfo_t fftsPlusDumpInfo;
    uint32_t descAddrType;
    uint32_t argsHandleInfoNum;
    void **argsHandleInfoPtr;
};

#pragma pack(pop)

static_assert(sizeof(rtStarsSqeHeader_t) == 8, "rtStarsSqeHeader_t must be 8 bytes");
static_assert(sizeof(rtFftsPlusSqe_t) == 64, "rtFftsPlusSqe_t must be 64 bytes");
static_assert(sizeof(rtFftsPlusComCtx_t) == 128, "rtFftsPlusComCtx_t must be 128 bytes");
static_assert(sizeof(rtFftsPlusSdmaCtx_t) == 128, "rtFftsPlusSdmaCtx_t must be 128 bytes");

extern "C" RTS_API rtError_t rtFftsPlusTaskLaunchWithFlag(rtFftsPlusTaskInfo_t *fftsPlusTaskInfo,
                                                           rtStream_t stm,
                                                           uint32_t flag);

#endif
