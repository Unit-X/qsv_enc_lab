//
// Created by Anders Cedronius on 2021-01-15.
//

// Watch the incoming video
// ffplay -f rawvideo -pixel_format nv12 -s 1280x720 source.yuv

//Watch the output
//ffplay output.h264

//Reproducing the out of GOP backward referencing pictures add MFX_GOP_STRICT to the GopOptFlag

// Without MFX_GOP_STRICT the output.h264 produces ->

/*
FRAME type B   PTS: 84600 DTS 84600 Frame number: 49
FRAME type P   PTS: 88200 DTS 86400 Frame number: 50
FRAME type IDR PTS: 90000 DTS 88200 Frame number: 51
FRAME type P   PTS: 95400 DTS 90000 Frame number: 52
FRAME type B   PTS: 91800 DTS 91800 Frame number: 53
FRAME type B   PTS: 93600 DTS 93600 Frame number: 54
 */

// With MFX_GOP_STRICT set the output.h264 produces ->

/*
FRAME type B   PTS: 82800 DTS 82800 Frame number: 48
FRAME type B   PTS: 84600 DTS 84600 Frame number: 49
FRAME type IDR PTS: 90000 DTS 86400 Frame number: 50
FRAME type B   PTS: 88200 DTS 88200 Frame number: 51
FRAME type P   PTS: 95400 DTS 90000 Frame number: 52
 */


#define ENCODE_NUMBER_FRAMES 55

#include <iostream>
#include <va/va_drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <memory>
#include <vector>
#include <algorithm>
#include <thread>
#include <cstring>

#include "mfxvideo++.h"

#define MSDK_ALIGN16(value)   (((value + 15) >> 4) << 4)
#define MSDK_ALIGN32(value)   (((value + 31) >> 5) << 5)

std::unique_ptr<MFXVideoENCODE> gQSVEncoder;

enum class PictureType {
    Unknown,
    IDR,
    P,
    B
};

const uint8_t* findNextNalUnit(const uint8_t *pStart,
                                               const uint8_t *pEnd,
                                               uint8_t *pNalCode,
                                               const uint8_t **ppNalPayload,
                                               bool *pEndReached) {
    const uint8_t *lpStart = pStart;
    while (lpStart <= pEnd - 3 && (lpStart[0] || lpStart[1] || lpStart[2] != 1)) {
        ++lpStart;
    }
    if (lpStart > pEnd - 3) {
        *pEndReached = true;
        return nullptr;
    }
    *pNalCode = lpStart[3];
    *ppNalPayload = lpStart + 3;
    if (lpStart > pStart && *(lpStart - 1) == 0) {
        return (lpStart - 1);
    }
    return lpStart;
}

PictureType getImageType(const uint8_t *pDataStart, size_t lDataSize) {

    if (lDataSize < 4) {
        return PictureType::Unknown;
    }

    const uint8_t *pDataEnd = pDataStart + lDataSize;
    uint8_t lNalCode;
    const uint8_t *nalPayload;
    bool lEndReached = false;
    const uint8_t* pNalEnd = findNextNalUnit(pDataStart, pDataEnd, &lNalCode, &nalPayload, &lEndReached) - 1 ;
    if (pNalEnd == nullptr) {
        return PictureType::Unknown;;
    }
    if (lEndReached) {
        return PictureType::Unknown;;
    }
    uint8_t lCurrentNalCode = lNalCode;
    while (true) {
        const uint8_t* pStartOfPayload = nalPayload;
        pNalEnd = findNextNalUnit(nalPayload, pDataEnd, &lNalCode, &nalPayload, &lEndReached);
        if (lEndReached == true) {
            pNalEnd = pDataEnd;
        }
        size_t lNalSize = pNalEnd - pStartOfPayload;

        uint8_t nal_ref_idc = lCurrentNalCode >> 5;
        lCurrentNalCode = lCurrentNalCode & 0x1f;

        if (lCurrentNalCode == 7 ) {
            //SPS
            // QSV_LOGGER(false,LOGG_NOTIFY,"Found SPS size > " << unsigned(lNalSize))
        } else if (lCurrentNalCode == 8 ) {
            //PPS
            // QSV_LOGGER(false,LOGG_NOTIFY,"Found PPS size > " << unsigned(lNalSize))
        } else if (lCurrentNalCode == 9) {
            // QSV_LOGGER(false,LOGG_NOTIFY,"AUD")
        } else if (lCurrentNalCode == 5) {
            //IDR
            return PictureType::IDR;
        } else if (lCurrentNalCode == 6) {
            //SEI
            // QSV_LOGGER(false,LOGG_NOTIFY,"SEI")
        } else if (lCurrentNalCode == 1 && nal_ref_idc == 1) {
            //P-Frame
            return PictureType::P;
        } else if (lCurrentNalCode == 1 && nal_ref_idc == 0) {
            //P-Frame
            //std::cout  << "B-Frame";
            return PictureType::B;
        }

        if (lEndReached) {
            break;
        }
        lCurrentNalCode = lNalCode;
    }
    return PictureType::Unknown;
}

int getFreeSurfaceIndex(const std::vector<mfxFrameSurface1>& pSurfacesPool)
{
    auto it = std::find_if(pSurfacesPool.begin(), pSurfacesPool.end(), [](const mfxFrameSurface1& surface) {
        return 0 == surface.Data.Locked;
    });

    if(it == pSurfacesPool.end())
        return MFX_ERR_NOT_FOUND;
    else return it - pSurfacesPool.begin();
}

int main() {
    std::cout << "Encoder started" << std::endl;

    // Open VA
    int lFd = open("/dev/dri/renderD128", O_RDWR);
    if (lFd < 1) {
        return EXIT_FAILURE;
    }
    VADisplay mVADisplay = vaGetDisplayDRM(lFd);
    if (!mVADisplay) {
        close(lFd);
        return EXIT_FAILURE;
    }

    int lMajor = 1, lMinor = 0;
    VAStatus lStatus = vaInitialize(mVADisplay, &lMajor, &lMinor);
    if (lStatus != VA_STATUS_SUCCESS) {
        close(lFd);
        return EXIT_FAILURE;
    }

    std::cout << "VA init OK" << std::endl;

    //OK VA init OK now init MSDK

    MFXVideoSession lSession;

    mfxStatus sts = MFX_ERR_NONE;
    mfxIMPL lMfxImpl = MFX_IMPL_AUTO_ANY;
    mfxVersion lMfxVer = {
            { 34, 1}
    };

    sts = lSession.Init(lMfxImpl, &lMfxVer);
    if (sts != MFX_ERR_NONE) {
        std::cout << "Unable to init MSDK version " << unsigned(lMfxVer.Major) << "." << unsigned(lMfxVer.Minor) << std::endl;
        return EXIT_FAILURE;
    }

    //Check if hardware or software is used
    mfxIMPL lImpl;
    sts = lSession.QueryIMPL(&lImpl);
    if (sts != MFX_ERR_NONE) {
        std::cout << "QueryIMPL failed -> " << sts << std::endl;
        return EXIT_FAILURE;
    }
    if (lImpl == MFX_IMPL_SOFTWARE) {
        std::cout << "MSDK implementation in software is not allowed " << std::endl;
        return EXIT_FAILURE;
    } else {
        sts = lSession.SetHandle(MFX_HANDLE_VA_DISPLAY, (mfxHDL)mVADisplay);
        if (sts != MFX_ERR_NONE) {
            std::cout << "SetHandle failed -> " << sts << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::cout << "MSDK init OK" << std::endl;

    gQSVEncoder = std::make_unique<MFXVideoENCODE>(lSession);
    if (gQSVEncoder == nullptr) {
        return EXIT_FAILURE;
    }

    // Set required video parameters for encode
    // - In this example we are encoding an AVC (H.264) stream
    mfxVideoParam mfxEncParams = {0};
    memset(&mfxEncParams, 0, sizeof(mfxEncParams));
    mfxEncParams.mfx.CodecId = MFX_CODEC_AVC;
    mfxEncParams.mfx.TargetUsage = MFX_TARGETUSAGE_BEST_QUALITY;
    mfxEncParams.mfx.TargetKbps = 1000;
    mfxEncParams.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    mfxEncParams.mfx.GopPicSize = 50;
    mfxEncParams.mfx.GopRefDist = 3;
    mfxEncParams.mfx.GopOptFlag = MFX_GOP_CLOSED;
    mfxEncParams.mfx.FrameInfo.FrameRateExtN = 50000;
    mfxEncParams.mfx.FrameInfo.FrameRateExtD = 1000;
    mfxEncParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    mfxEncParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    mfxEncParams.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    mfxEncParams.mfx.FrameInfo.CropX = 0;
    mfxEncParams.mfx.FrameInfo.CropY = 0;
    mfxEncParams.mfx.FrameInfo.CropW = 1280;
    mfxEncParams.mfx.FrameInfo.CropH = 720;
    // Width must be a multiple of 16
    // Height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    mfxEncParams.mfx.FrameInfo.Width = MSDK_ALIGN16(1280);
    mfxEncParams.mfx.FrameInfo.Height =
            (MFX_PICSTRUCT_PROGRESSIVE == mfxEncParams.mfx.FrameInfo.PicStruct) ?
            MSDK_ALIGN16(720) :
            MSDK_ALIGN32(720);

    mfxEncParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;


    // Validate video encode parameters (optional)
    // - In this example the validation result is written to same structure
    // - MFX_WRN_INCOMPATIBLE_VIDEO_PARAM is returned if some of the video parameters are not supported,
    //   instead the encoder will select suitable parameters closest matching the requested configuration
    sts = gQSVEncoder->Query(&mfxEncParams, &mfxEncParams);
    if (sts != MFX_ERR_NONE) {
        std::cout << "Query failed -> " << sts << std::endl;
        return EXIT_FAILURE;
    }

    // Query number of required surfaces for encoder
    mfxFrameAllocRequest EncRequest = {0};
    sts = gQSVEncoder->QueryIOSurf(&mfxEncParams, &EncRequest);
    if (sts != MFX_ERR_NONE) {
        std::cout << "QueryIOSurf failed -> " << sts << std::endl;
        return EXIT_FAILURE;
    }

    mfxU16 nEncSurfNum = EncRequest.NumFrameSuggested;

    // Allocate surfaces for encoder
    // - Width and height of buffer must be aligned, a multiple of 32
    // - Frame surface array keeps pointers all surface planes and general frame info
    mfxU16 width = (mfxU16) MSDK_ALIGN32(EncRequest.Info.Width);
    mfxU16 height = (mfxU16) MSDK_ALIGN32(EncRequest.Info.Height);
    mfxU8 bitsPerPixel = 12;        // NV12 format is a 12 bits per pixel format
    mfxU32 surfaceSize = width * height * bitsPerPixel / 8;
    std::vector<mfxU8> surfaceBuffersData(surfaceSize * nEncSurfNum, 0);
    mfxU8* surfaceBuffers = surfaceBuffersData.data();

    // Allocate surface headers (mfxFrameSurface1) for encoder
    std::vector<mfxFrameSurface1> pEncSurfaces(nEncSurfNum, {0});
    for (int i = 0; i < nEncSurfNum; i++) {
        pEncSurfaces[i].Info = mfxEncParams.mfx.FrameInfo;
        pEncSurfaces[i].Data.Y = &surfaceBuffers[surfaceSize * i];
        pEncSurfaces[i].Data.U = pEncSurfaces[i].Data.Y + width * height;
        pEncSurfaces[i].Data.V = pEncSurfaces[i].Data.U + 1;
        pEncSurfaces[i].Data.Pitch = width;
    }

    // Initialize the Media SDK encoder
    sts = gQSVEncoder->Init(&mfxEncParams);
    if (sts != MFX_ERR_NONE) {
        std::cout << "Init failed -> " << sts << std::endl;
        return EXIT_FAILURE;
    }

    // Retrieve video parameters selected by encoder.
    // - BufferSizeInKB parameter is required to set bit stream buffer size
    mfxVideoParam par = {0};
    sts = gQSVEncoder->GetVideoParam(&par);
    if (sts != MFX_ERR_NONE) {
        std::cout << "GetVideoParam failed -> " << sts << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<uint8_t> lYuvData;
    std::ifstream lFile;
    //Support a build directory or someone built in the root.
    lFile.open ("../source.yuv", std::ios::in | std::ios::binary | std::ios::ate);
    if (!lFile) {
        std::cout << "Unable reading YUV file" << std::endl;
        return EXIT_FAILURE;
    }
    size_t lSize = lFile.tellg();
    lYuvData.resize(lSize);
    lFile.seekg (0, std::ios::beg);
    lFile.read ((char*)lYuvData.data(), lSize);
    lFile.close();

    // Prepare Media SDK bit stream buffer
    mfxBitstream mfxBS = {0};
    mfxBS.MaxLength = par.mfx.BufferSizeInKB * 1000;
    std::vector<mfxU8> bstData(mfxBS.MaxLength);
    mfxBS.Data = bstData.data();


    // ===================================
    // Start encoding the frames
    //

    int nEncSurfIdx = 0;
    mfxSyncPoint syncp;
    mfxU32 nFrame = 0;

    //
    // Stage 1: Main encoding loop
    //

    uint64_t lPts = 0;
    uint64_t lPtsAdvance =(uint64_t)(90000.0/((double)mfxEncParams.mfx.FrameInfo.FrameRateExtN/(double)mfxEncParams.mfx.FrameInfo.FrameRateExtD));
    uint64_t lFrameCounter = ENCODE_NUMBER_FRAMES;
    uint64_t lCodeStreamCounter = 0;

    std::ofstream lFileOut;
    lFileOut.open ("../output.h264", std::ios::out | std::ios::binary);
    if (!lFileOut) {
        std::cout << "Failed creating output file" << std::endl;
        return EXIT_FAILURE;
    }

    while (MFX_ERR_NONE <= sts || MFX_ERR_MORE_DATA == sts) {
        nEncSurfIdx = getFreeSurfaceIndex(pEncSurfaces);   // Find free frame surface
        if (nEncSurfIdx == MFX_ERR_NOT_FOUND) {
            std::this_thread::sleep_for(std::chrono::milliseconds (1));
            continue;
        }

        if (!lFrameCounter--) {
            //std::cout << "Ending encoder." << std::endl;
            break;
        }

        pEncSurfaces[nEncSurfIdx].Info.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        pEncSurfaces[nEncSurfIdx].Data.TimeStamp = lPts;
        lPts += lPtsAdvance;
        unsigned char *pDataY = pEncSurfaces[nEncSurfIdx].Data.Y;
        unsigned char *pDataUV = pEncSurfaces[nEncSurfIdx].Data.U;
        int lNumYsamples = 1280 * 720;
        int lNumUVsamples = lNumYsamples / 2;
        memcpy(pDataY, lYuvData.data(), lNumYsamples);
        memcpy(pDataUV, lYuvData.data() + lNumYsamples, lNumUVsamples);

        for (;;) {
            // Encode a frame asychronously (returns immediately)
            sts = gQSVEncoder->EncodeFrameAsync(NULL, &pEncSurfaces[nEncSurfIdx], &mfxBS, &syncp);

            if (MFX_ERR_NONE < sts && !syncp) {     // Repeat the call if warning and no output
                if (MFX_WRN_DEVICE_BUSY == sts)
                    std::this_thread::sleep_for(std::chrono::milliseconds (10));  // Wait if device is busy, then repeat the same call
            } else if (MFX_ERR_NONE < sts && syncp) {
                sts = MFX_ERR_NONE;     // Ignore warnings if output is available
                break;
            } else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
                // Allocate more bitstream buffer memory here if needed...
                break;
            } else
                break;
        }

        if (MFX_ERR_NONE == sts) {
            sts = lSession.SyncOperation(syncp, 1000);      // Synchronize. Wait until encoded frame is ready
            if (sts == MFX_ERR_NONE) {
                PictureType lPicType=getImageType(mfxBS.Data, mfxBS.DataLength);
                std::cout << "FRAME type ";
                if (lPicType == PictureType::IDR) {
                    std::cout << "IDR ";
                } else if (lPicType == PictureType::P) {
                    std::cout << "P   ";
                } else if (lPicType == PictureType::B) {
                    std::cout << "B   ";
                } else {
                    std::cout << "Unknown ";
                }
                std::cout << "PTS: " << mfxBS.TimeStamp << " DTS " << mfxBS.DecodeTimeStamp << " Frame number: " << ++lCodeStreamCounter <<  std::endl;
                lFileOut.write ((char*)mfxBS.Data, mfxBS.DataLength);
                mfxBS.DataLength = 0;
                mfxBS.DataOffset = 0;
            }
        }
    }

    if (sts == MFX_ERR_MORE_DATA) {
        sts = MFX_ERR_NONE;
    }

    //
    // Stage 2: Retrieve the buffered encoded frames
    //
    while (MFX_ERR_NONE <= sts) {
        for (;;) {
            // Encode a frame asychronously (returns immediately)
            sts = gQSVEncoder->EncodeFrameAsync(NULL, NULL, &mfxBS, &syncp);

            if (MFX_ERR_NONE < sts && !syncp) {     // Repeat the call if warning and no output
                if (MFX_WRN_DEVICE_BUSY == sts)
                    std::this_thread::sleep_for(std::chrono::milliseconds (10)); ;  // Wait if device is busy, then repeat the same call
            } else if (MFX_ERR_NONE < sts && syncp) {
                sts = MFX_ERR_NONE;     // Ignore warnings if output is available
                break;
            } else
                break;
        }

        if (MFX_ERR_NONE == sts) {
            sts = lSession.SyncOperation(syncp, 1000);      // Synchronize. Wait until encoded frame is ready
            if (sts == MFX_ERR_NONE) {
                PictureType lPicType=getImageType(mfxBS.Data, mfxBS.DataLength);
                std::cout << "FRAME type ";
                if (lPicType == PictureType::IDR) {
                    std::cout << "IDR ";
                } else if (lPicType == PictureType::P) {
                    std::cout << "P   ";
                } else if (lPicType == PictureType::B) {
                    std::cout << "B   ";
                } else {
                    std::cout << "Unknown ";
                }
                std::cout << "PTS: " << mfxBS.TimeStamp << " DTS " << mfxBS.DecodeTimeStamp << " Frame number: " << ++lCodeStreamCounter <<  std::endl;
                lFileOut.write ((char*)mfxBS.Data, mfxBS.DataLength);
                mfxBS.DataLength = 0;
                mfxBS.DataOffset = 0;
            }
        }
    }

    // ===================================================================
    // Clean up resources
    //  - It is recommended to close Media SDK components first, before releasing allocated surfaces, since
    //    some surfaces may still be locked by internal Media SDK resources.
    gQSVEncoder->Close();
    lSession.Close();
    close(lFd);
    lFileOut.close();
    std::cout << "Encoder exit" << std::endl;
    return EXIT_SUCCESS;
}


