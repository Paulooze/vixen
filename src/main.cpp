#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "nvCVImage.h"
#include "nvVideoEffects.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

char *g_nvVFXSDKPath = nullptr;

namespace
{

    void failNv(const char *what, NvCV_Status status)
    {
        std::cerr << what << " failed with NvCV status "
                  << static_cast<int>(status) << '\n';
        std::exit(1);
    }

    void checkNv(const char *what, NvCV_Status status)
    {
        if (status != NVCV_SUCCESS)
            failNv(what, status);
    }

    struct Image
    {
        NvCVImage image{};

        ~Image()
        {
            // Only use this for images allocated by NvCVImage_Alloc.
            if (image.pixels)
                NvCVImage_Dealloc(&image);
        }

        Image() = default;
        Image(const Image &) = delete;
        Image &operator=(const Image &) = delete;
    };

    struct Effect
    {
        NvVFX_Handle handle = nullptr;

        ~Effect()
        {
            if (handle)
                NvVFX_DestroyEffect(handle);
        }

        Effect() = default;
        Effect(const Effect &) = delete;
        Effect &operator=(const Effect &) = delete;
    };

    unsigned outputWidthForHeight(
        unsigned inputWidth,
        unsigned inputHeight,
        unsigned outputHeight)
    {
        // Preserve aspect ratio.
        // Round to nearest integer.
        return static_cast<unsigned>(
            (static_cast<uint64_t>(inputWidth) * outputHeight +
             inputHeight / 2) /
            inputHeight);
    }

    enum class ReadResult
    {
        Ok,
        Eof,
        Short
    };

    ReadResult readFrame(
        FILE *f,
        std::vector<uint8_t> &data)
    {
        size_t total = 0;

        while (total < data.size())
        {
            const size_t got = std::fread(
                data.data() + total,
                1,
                data.size() - total,
                f);

            if (got == 0)
            {
                if (std::feof(f))
                    return total == 0
                               ? ReadResult::Eof
                               : ReadResult::Short;

                return ReadResult::Short;
            }

            total += got;
        }

        return ReadResult::Ok;
    }

    bool writeFrame(
        FILE *f,
        const std::vector<uint8_t> &data)
    {
        size_t total = 0;

        while (total < data.size())
        {
            const size_t written = std::fwrite(
                data.data() + total,
                1,
                data.size() - total,
                f);

            if (written == 0)
                return false;

            total += written;
        }

        return true;
    }
} // namespace

int main(int argc, char **argv)
{

    if (argc != 4)
    {
        std::cerr
            << "Usage:\n"
            << "  maxine-vfx-pipe.exe "
            << "<width> <height> <output-height>\n\n"
            << "Reads BGR24 frames from stdin and writes "
            << "BGR24 frames to stdout.\n\n"
            << "Example:\n"
            << "  maxine-vfx-pipe.exe 1280 720 1080\n";

        return 2;
    }

    const unsigned inputWidth =
        static_cast<unsigned>(std::stoul(argv[1]));

    const unsigned inputHeight =
        static_cast<unsigned>(std::stoul(argv[2]));

    const unsigned outputHeight =
        static_cast<unsigned>(std::stoul(argv[3]));

    if (!inputWidth || !inputHeight || !outputHeight)
    {
        std::cerr << "Dimensions must be non-zero.\n";
        return 2;
    }

    const unsigned outputWidth =
        outputWidthForHeight(
            inputWidth,
            inputHeight,
            outputHeight);

    const size_t inputBytes =
        static_cast<size_t>(inputWidth) *
        inputHeight * 3;

    const size_t outputBytes =
        static_cast<size_t>(outputWidth) *
        outputHeight * 3;

    std::vector<uint8_t> input(inputBytes);
    std::vector<uint8_t> output(outputBytes);

    std::cerr
        << "Input:  "
        << inputWidth << 'x' << inputHeight
        << " BGR24 (" << inputBytes << " bytes)\n";

    std::cerr
        << "Output: "
        << outputWidth << 'x' << outputHeight
        << " BGR24 (" << outputBytes << " bytes)\n";

    // -----------------------------------------------------------------
    // Wrap the ordinary CPU memory as NvCVImage objects.
    // NvCVImage_Init does NOT own/allocate this memory.
    // -----------------------------------------------------------------

    NvCVImage srcCpu{};
    NvCVImage dstCpu{};

    checkNv(
        "NvCVImage_Init(srcCpu)",
        NvCVImage_Init(
            &srcCpu,
            inputWidth,
            inputHeight,
            static_cast<int>(inputWidth * 3),
            input.data(),
            NVCV_BGR,
            NVCV_U8,
            NVCV_CHUNKY,
            NVCV_CPU));

    checkNv(
        "NvCVImage_Init(dstCpu)",
        NvCVImage_Init(
            &dstCpu,
            outputWidth,
            outputHeight,
            static_cast<int>(outputWidth * 3),
            output.data(),
            NVCV_BGR,
            NVCV_U8,
            NVCV_CHUNKY,
            NVCV_CPU));

    // -----------------------------------------------------------------
    // GPU buffers.
    //
    // These mirror UpscalePipelineApp's existing buffer formats.
    // -----------------------------------------------------------------

    Image srcGpu;
    Image arGpu;
    Image rgbaGpu;
    Image dstGpu;
    Image tmpGpu;

    checkNv(
        "NvCVImage_Alloc(srcGpu)",
        NvCVImage_Alloc(
            &srcGpu.image,
            inputWidth,
            inputHeight,
            NVCV_BGR,
            NVCV_F32,
            NVCV_PLANAR,
            NVCV_GPU,
            1));

    checkNv(
        "NvCVImage_Alloc(arGpu)",
        NvCVImage_Alloc(
            &arGpu.image,
            inputWidth,
            inputHeight,
            NVCV_BGR,
            NVCV_F32,
            NVCV_PLANAR,
            NVCV_GPU,
            1));

    checkNv(
        "NvCVImage_Alloc(rgbaGpu)",
        NvCVImage_Alloc(
            &rgbaGpu.image,
            inputWidth,
            inputHeight,
            NVCV_RGBA,
            NVCV_U8,
            NVCV_INTERLEAVED,
            NVCV_GPU,
            32));

    checkNv(
        "NvCVImage_Alloc(dstGpu)",
        NvCVImage_Alloc(
            &dstGpu.image,
            outputWidth,
            outputHeight,
            NVCV_RGBA,
            NVCV_U8,
            NVCV_INTERLEAVED,
            NVCV_GPU,
            32));

    checkNv(
        "NvCVImage_Alloc(tmpGpu)",
        NvCVImage_Alloc(
            &tmpGpu.image,
            outputWidth,
            outputHeight,
            NVCV_BGR,
            NVCV_U8,
            NVCV_CHUNKY,
            NVCV_GPU,
            0));

    NvCVImage *tmp = &tmpGpu.image;

    // -----------------------------------------------------------------
    // Create effects.
    // -----------------------------------------------------------------

    Effect artifactReduction;
    Effect upscale;

    checkNv(
        "NvVFX_CreateEffect(Artifact Reduction)",
        NvVFX_CreateEffect(
            NVVFX_FX_ARTIFACT_REDUCTION,
            &artifactReduction.handle));

    checkNv(
        "NvVFX_CreateEffect(Super Resolution)",
        NvVFX_CreateEffect(
            NVVFX_FX_SR_UPSCALE,
            &upscale.handle));

    CUstream stream = 0;

    // -----------------------------------------------------------------
    // Artifact reduction setup.
    // Mirrors UpscalePipelineApp.
    // -----------------------------------------------------------------

    checkNv(
        "NvVFX_SetImage(AR input)",
        NvVFX_SetImage(
            artifactReduction.handle,
            NVVFX_INPUT_IMAGE,
            &srcGpu.image));

    checkNv(
        "NvVFX_SetImage(AR output)",
        NvVFX_SetImage(
            artifactReduction.handle,
            NVVFX_OUTPUT_IMAGE,
            &arGpu.image));

    checkNv(
        "NvVFX_SetCudaStream(AR)",
        NvVFX_SetCudaStream(
            artifactReduction.handle,
            NVVFX_CUDA_STREAM,
            stream));

    checkNv(
        "NvVFX_SetU32(AR mode)",
        NvVFX_SetU32(
            artifactReduction.handle,
            NVVFX_MODE,
            0));

    // -----------------------------------------------------------------
    // Upscaler setup.
    // -----------------------------------------------------------------

    checkNv(
        "NvVFX_SetImage(SR input)",
        NvVFX_SetImage(
            upscale.handle,
            NVVFX_INPUT_IMAGE,
            &rgbaGpu.image));

    checkNv(
        "NvVFX_SetImage(SR output)",
        NvVFX_SetImage(
            upscale.handle,
            NVVFX_OUTPUT_IMAGE,
            &dstGpu.image));

    checkNv(
        "NvVFX_SetCudaStream(SR)",
        NvVFX_SetCudaStream(
            upscale.handle,
            NVVFX_CUDA_STREAM,
            stream));

    // Equivalent to --upscale_strength=0 for the first test.
    checkNv(
        "NvVFX_SetF32(SR strength)",
        NvVFX_SetF32(
            upscale.handle,
            NVVFX_STRENGTH,
            0.0f));

    std::cerr << "Loading Maxine models...\n";

    checkNv(
        "NvVFX_Load(Artifact Reduction)",
        NvVFX_Load(artifactReduction.handle));

    checkNv(
        "NvVFX_Load(Super Resolution)",
        NvVFX_Load(upscale.handle));

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    fprintf(stderr, "Maxine ready. Processing stream...\n");

    uint64_t frameNum = 0;

    for (;;)
    {
        const ReadResult result = readFrame(stdin, input);

        if (result == ReadResult::Eof)
            break;

        if (result == ReadResult::Short)
        {
            std::cerr
                << "Input ended in the middle of frame "
                << frameNum << '\n';
            return 1;
        }

        checkNv(
            "NvVFX_Load(Artifact Reduction)",
            NvVFX_Load(artifactReduction.handle));

        checkNv(
            "NvVFX_Load(Super Resolution)",
            NvVFX_Load(upscale.handle));

        // CPU BGR8 -> GPU planar BGR F32.
        checkNv(
            "NvCVImage_Transfer(input)",
            NvCVImage_Transfer(
                &srcCpu,
                &srcGpu.image,
                1.f / 255.f,
                stream,
                tmp));

        // Artifact reduction.
        checkNv(
            "NvVFX_Run(Artifact Reduction)",
            NvVFX_Run(
                artifactReduction.handle,
                0));

        // AR planar BGR F32 -> SR interleaved RGBA U8.
        checkNv(
            "NvCVImage_Transfer(AR -> RGBA)",
            NvCVImage_Transfer(
                &arGpu.image,
                &rgbaGpu.image,
                255.f,
                stream,
                tmp));

        // Super resolution.
        checkNv(
            "NvVFX_Run(Super Resolution)",
            NvVFX_Run(
                upscale.handle,
                0));

        // GPU RGBA8 -> CPU BGR8.
        checkNv(
            "NvCVImage_Transfer(output)",
            NvCVImage_Transfer(
                &dstGpu.image,
                &dstCpu,
                1.f,
                stream,
                tmp));

        if (!writeFrame(stdout, output))
        {
            std::cerr
                << "Failed writing output frame "
                << frameNum << '\n';
            return 1;
        }

        ++frameNum;

        if ((frameNum % 100) == 0)
        {
            std::cerr
                << "Processed "
                << frameNum
                << " frames\n";
        }
    }

    std::cerr
        << "Done. Processed "
        << frameNum
        << " frames.\n";

    return 0;
}
