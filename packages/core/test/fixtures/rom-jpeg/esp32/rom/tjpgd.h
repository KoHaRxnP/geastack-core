// Host stand-in for the ROM API. The test supplies its implementation; this is
// deliberately not a replacement JPEG decoder or a hardware performance test.
#pragma once
#include <cstdint>
using UINT = unsigned int;
using BYTE = unsigned char;
enum JRESULT { JDR_OK, JDR_INTR, JDR_INP, JDR_MEM1, JDR_MEM2, JDR_PAR, JDR_FMT1, JDR_FMT2, JDR_FMT3 };
struct JRECT { std::uint16_t left, right, top, bottom; };
struct JDEC {
    UINT width = 0, height = 0;
    void *device = nullptr;
};
extern "C" JRESULT jd_prepare(JDEC *, UINT (*)(JDEC *, BYTE *, UINT), void *, UINT, void *);
extern "C" JRESULT jd_decomp(JDEC *, UINT (*)(JDEC *, void *, JRECT *), BYTE);
