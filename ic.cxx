//
// Image Conversion app
//

#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <shlwapi.h>
#include <wrl.h>
#include <mferror.h>
#include <psapi.h>

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <ppl.h>

#include <chrono>
#include <memory>

using namespace std;
using namespace std::chrono;
using namespace concurrency;
using namespace Microsoft::WRL;

#include <djltrace.hxx>
#include <djlenum.hxx>
#include <djl_pa.hxx>
#include <djltimed.hxx>
#include <djl_wav.hxx>

#pragma comment( lib, "ole32.lib" )
#pragma comment( lib, "shlwapi.lib" )
#pragma comment( lib, "oleaut32.lib" )
#pragma comment( lib, "windowscodecs.lib" )

CDJLTrace tracer;
ComPtr<IWICImagingFactory> g_IWICFactory;
long long g_CollagePrepTime = 0;
long long g_CollageStitchTime = 0;
long long g_CollageStitchFloodTime = 0;
long long g_CollageStitchCopyPixelsTime = 0;
long long g_CollageStitchDrawTime = 0;
long long g_CollageWriteTime = 0;

// 24bppBGR is convenient because:
//    -- encoders support this format, and they don't all (e.g. JPG) support RGB
//    -- it's the native format when decoding a JPG, so no conversion is needed
// The only exception in this app is non-Collage convert 48bppRGB TIFF to another 48bppRGB TIFF
// In all other cases, bitmaps are always 24bppBGR

const WICPixelFormatGUID g_GuidPixelFormat = GUID_WICPixelFormat24bppBGR;
const int g_BitsPerPixel = 24;

struct BitmapDimensions
{
    UINT width;
    UINT height;
};

void PrintGuid( GUID & guid )
{
    printf( "Guid = {%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
            guid.Data1, guid.Data2, guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7] );
} //PrintGuid

void PrintPixelFormatInfo( ComPtr<IWICBitmapSource> & source )
{
    WICPixelFormatGUID pf;
    HRESULT hr = source->GetPixelFormat( &pf );
    if ( FAILED( hr ) )
    {
        printf( "can't get pixel format %#x\n", hr );
        return;
    }

    printf( "  " );
    PrintGuid( pf );
    if ( GUID_WICPixelFormat24bppBGR == pf )
        printf( "  GUID_WICPixelFormat24bppBGR" );
    else if ( GUID_WICPixelFormat24bppRGB == pf )
        printf( "  GUID_WICPixelFormat24bppRGB" );
    else if ( GUID_WICPixelFormat48bppRGB == pf )
        printf( "  GUID_WICPixelFormat48bppRGB" );
    printf( "\n" );

    ComPtr<IWICComponentInfo> ci;
    hr = g_IWICFactory->CreateComponentInfo( pf, ci.GetAddressOf() );
    if ( FAILED( hr ) )
    {
        printf( "can't create component info %#x\n", hr );
        return;
    }

    ComPtr<IWICPixelFormatInfo> pfi;
    hr = ci->QueryInterface( __uuidof( IWICPixelFormatInfo ), (void **) pfi.GetAddressOf() );
    if ( FAILED( hr ) )
    {
        printf( "can't qi for pixelformatinfo %#x\n", hr );
        return;
    }

    UINT bpp;
    hr = pfi->GetBitsPerPixel( &bpp );
    if ( FAILED( hr ) )
    {
        printf( "can't getbitsperpixel %#x\n", hr );
        return;
    }

    UINT channels;
    hr = pfi->GetChannelCount( &channels );
    if ( FAILED( hr ) )
    {
        printf( "can't getchannelcount %#x\n", hr );
        return;
    }

#if false
    printf( "  bits per pixel %d, channel count %d\n", bpp, channels );

    for ( UINT c = 0; c < channels; c++ )
    {
        ULONG mask = 0;
        UINT size = 0;
        hr = pfi->GetChannelMask( c, sizeof mask, (BYTE *) &mask, &size );

        printf( "    channel # %d, hr %#x, mask %#x\n", c, hr, mask );
    }
#endif
} //PrintPixelFormatInfo

bool SameDouble( double a, double b )
{
    return ( fabs( a - b ) < 0.01 );
} //SameDouble

HRESULT ConvertBitmapTo24bppBGROr48bppRGB( ComPtr<IWICBitmapSource> & source, bool force24bppBGR )
{
    WICPixelFormatGUID pixelFormat;
    HRESULT hr = source->GetPixelFormat( &pixelFormat );

    if ( FAILED( hr ) )
    {
        printf( "GetPixelFormat failed with error %#x\n", hr );
        return hr;
    }

    if ( g_GuidPixelFormat == pixelFormat )
    {
        //printf( "no need to convert wic pixel format\n" );
        return S_OK;
    }

    if ( !force24bppBGR && ( GUID_WICPixelFormat48bppRGB == pixelFormat ) )
    {
        printf( "bitmap source is 48bppRGB and that's OK!\n" );
        return S_OK;
    }

    //printf( "yes, need to convert wic pixel format to 24bppBGR from:\n" );
    //PrintPixelFormatInfo( source );

    ComPtr<IWICFormatConverter> converter;
    hr = g_IWICFactory->CreateFormatConverter( converter.GetAddressOf() );

    if ( SUCCEEDED( hr ) )
    {
        hr = converter->Initialize( source.Get(), g_GuidPixelFormat, WICBitmapDitherTypeNone, NULL, 0.0f, WICBitmapPaletteTypeCustom );
        if ( SUCCEEDED( hr ) )
        {
            ComPtr<IWICBitmapSource> newSource;
            hr = converter->QueryInterface( IID_IWICBitmapSource, (void **) newSource.GetAddressOf() );

            if ( SUCCEEDED( hr ) )
            {
                source.Reset();
                source.Attach( newSource.Detach() );

                //printf( "converted pixel format:\n" );
                //PrintPixelFormatInfo( source );
            }
            else
                printf( "can't qi for iwicbitmapsource: %#x\n", hr );
        }
        else
            printf( "failed to set pixel format of input: %#x\n", hr );
    }
    else
        printf( "can't CreateFormatConverter(): %#x\n", hr );

    return hr;
} //ConvertBitmapTo24bppBGROr48bppRGB

HRESULT LoadWICBitmap( WCHAR const * pwcPath, ComPtr<IWICBitmapSource> & source, ComPtr<IWICBitmapFrameDecode> & frame, bool force24bppBGR )
{
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = S_OK;

    hr = g_IWICFactory->CreateDecoderFromFilename( pwcPath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf() );

    if ( FAILED( hr ) )
    {
        if ( WINCODEC_ERR_COMPONENTINITIALIZEFAILURE == hr )
            printf( "  WINCODEC_ERR_COMPONENTINITIALIZEFAILURE:" );
        printf( "  hr from CreateDecoderFromFilename: %#x for %ws\n", hr, pwcPath );
        return hr;
    }

    hr = decoder->GetFrame( 0, frame.GetAddressOf() );

    if ( SUCCEEDED( hr ) )
        hr = frame->QueryInterface( IID_IWICBitmapSource, reinterpret_cast<void **> ( source.GetAddressOf() ) );

    // Convert to a smaller format to reduce RAM usage and make it something the JPG encoder is known to accept.

    if ( SUCCEEDED( hr ) )
        hr = ConvertBitmapTo24bppBGROr48bppRGB( source, force24bppBGR );

    return hr;
} //LoadWICBitmap

HRESULT CopyMetadata( ComPtr<IWICBitmapFrameEncode> encoder, ComPtr<IWICBitmapFrameDecode> & decoder )
{
    ComPtr<IWICMetadataBlockWriter> blockWriter;
    ComPtr<IWICMetadataBlockReader> blockReader;
    
    HRESULT hr = decoder->QueryInterface( IID_IWICMetadataBlockReader, (void **) blockReader.GetAddressOf() );
    if ( FAILED( hr ) )
        printf( "can't qi block reader: %#x\n", hr );

    if ( SUCCEEDED( hr ) )
    {
        hr = encoder->QueryInterface( IID_IWICMetadataBlockWriter, (void **) blockWriter.GetAddressOf() );
        if ( FAILED( hr ) )
            printf( "can't qi block writer: %#x\n", hr );
    }
    
    if ( SUCCEEDED( hr ) )
    {
        hr = blockWriter->InitializeFromBlockReader( blockReader.Get() );
        if ( FAILED( hr ) )
            printf( "can't initializefromblockreader (copy metadata): %#x\n", hr );
    }
    else if ( E_NOINTERFACE == hr )
    {
        // bmp files do this -- they have no metadata block and so don't support the interface

        hr = S_OK;
    }
    
    return hr;
} //CopyMetadata

HRESULT ScaleWICBitmap( ComPtr<IWICBitmapSource> & source, int longEdge )
{
    UINT width, height;

    HRESULT hr = source->GetSize( &width, &height );
    if ( FAILED( hr ) )
    {
        printf( "can't get size of input bitmap: %#x\n", hr );
        return hr;
    }

    UINT targetWidth, targetHeight;

    if ( width > height )
    {
        targetWidth = longEdge;
        targetHeight = (UINT) round( (double) longEdge / (double) width * (double) height );
    }
    else
    {
        targetHeight = longEdge;
        targetWidth = (UINT) round( (double) longEdge / (double) height * (double) width );
    }

    //printf( "original dimensions %u by %u, target %u by %u\n", width, height, targetWidth, targetHeight );

    ComPtr<IWICBitmapScaler> scaler;
    hr = g_IWICFactory->CreateBitmapScaler( scaler.GetAddressOf() );
    if ( FAILED( hr ) )
    {
        printf( "can't create a scaler: %#x\n", hr );
        return hr;
    }

    hr = scaler->Initialize( source.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeHighQualityCubic );
    if ( FAILED( hr ) )
    {
        printf( "can't initialize scaler: %#x\n", hr );
        return hr;
    }

    source.Reset();
    source.Attach( scaler.Detach() );

    return hr;
} //ScaleWICBitmap

static int StrideInBytes( int width, int bitsPerPixel )
{
    // Not sure if it's documented anywhere, but Windows seems to use 4-byte-aligned strides.
    // Stride is the number of bytes per row of an image. The last bytes to get to 4-byte alignment are unused.
    // Based on Internet searches, some other platforms use 8-byte-aligned strides.
    // I don't know how to query the runtime environment to tell what the default stride alignment is. Assume 4.

    int bytesPerPixel = bitsPerPixel / 8;

    if ( 0 != ( bitsPerPixel % 8 ) )
        bytesPerPixel++;

    const int AlignmentForStride = 4;

    return (((width * bytesPerPixel) + (AlignmentForStride - 1)) / AlignmentForStride) * AlignmentForStride;
} //StrideInBytes

HRESULT CreateWICEncoder( WCHAR const * pwcPath, ComPtr<IWICBitmapEncoder> & encoder,
                          ComPtr<IWICBitmapFrameEncode> & bitmapFrameEncode, WCHAR const * outputMimetype,
                          bool lowQualityOutput )
{
    ComPtr<IWICStream> stream;
    HRESULT hr = g_IWICFactory->CreateStream( stream.GetAddressOf() );
    if ( FAILED( hr ) )
    {
        printf( "can't create stream from factory, error %#x\n", hr );
        return hr;
    }

    hr = stream->InitializeFromFilename( pwcPath, GENERIC_WRITE );
    if ( FAILED( hr ) )
    {
        printf( "can't initialize from filename, error %#x, path %ws\n", hr, pwcPath );
        return hr;
    }

    if ( !wcscmp( outputMimetype, L"image/bmp" ) )
        hr = g_IWICFactory->CreateEncoder( GUID_ContainerFormatBmp, NULL, encoder.GetAddressOf() );
    else if ( !wcscmp( outputMimetype, L"image/gif" ) )
        hr = g_IWICFactory->CreateEncoder( GUID_ContainerFormatGif, NULL, encoder.GetAddressOf() );
    else if ( !wcscmp( outputMimetype, L"image/jpeg" ) )
        hr = g_IWICFactory->CreateEncoder( GUID_ContainerFormatJpeg, NULL, encoder.GetAddressOf() );
    else if ( !wcscmp( outputMimetype, L"image/png" ) )
        hr = g_IWICFactory->CreateEncoder( GUID_ContainerFormatPng, NULL, encoder.GetAddressOf() );
    else if ( !wcscmp( outputMimetype, L"image/tiff" ) )
        hr = g_IWICFactory->CreateEncoder( GUID_ContainerFormatTiff, NULL, encoder.GetAddressOf() );
    else
    {
        printf( "unexpected output mimetype %ws\n", outputMimetype );
        hr = STATUS_INVALID_PARAMETER;
    }

    if ( FAILED( hr ) )
    {
        printf( "can't create encoder, error %#x\n", hr );
        return hr;
    }

    hr = encoder->Initialize( stream.Get(), WICBitmapEncoderNoCache );
    if ( FAILED( hr ) )
    {
        printf( "can't initialize encoder, error %#x\n", hr );
        return hr;
    }

    ComPtr<IPropertyBag2> propertyBag;
    hr = encoder->CreateNewFrame( bitmapFrameEncode.GetAddressOf(), propertyBag.GetAddressOf() );
    if ( FAILED( hr ) )
    {
        printf( "can't create new frame, error %#x\n", hr );
        return hr;
    }

    if ( false ) // useful for debugging
    {
        ULONG cProps = 0;
        HRESULT hrProps = propertyBag->CountProperties( & cProps );

        if ( SUCCEEDED( hrProps ) )
        {
            printf( "there are %d properties\n", cProps );

            for ( ULONG i = 0; i < cProps; i++ )
            {
                ULONG read;
                PROPBAG2 bag;
                hrProps = propertyBag->GetPropertyInfo( i, 1, &bag, &read );
                if ( SUCCEEDED( hrProps ) )
                {
                    printf( "  property name %ws\n", bag.pstrName );
                    CoTaskMemFree( bag.pstrName );
                }
            }
        }
    }

    if ( !wcscmp( outputMimetype, L"image/tiff" ) )
    {
        PROPBAG2 optionQuality = { 0 };
        optionQuality.pstrName = L"CompressionQuality";
        VARIANT varValueQuality;
        VariantInit( &varValueQuality );
        varValueQuality.vt = VT_R4;

        if ( lowQualityOutput )
            varValueQuality.fltVal = 0.6;
        else
            varValueQuality.fltVal = 1.0;

        hr = propertyBag->Write( 1, &optionQuality, &varValueQuality );
        if ( FAILED( hr ) )
        {
            printf( "can't write compressionquality to property bag, error %#x\n", hr );
            return hr;
        }
    }

    if ( !wcscmp( outputMimetype, L"image/jpeg" ) )
    {
        PROPBAG2 optionQuality = { 0 };
        optionQuality.pstrName = L"ImageQuality";
        VARIANT varValueQuality;
        VariantInit( &varValueQuality );
        varValueQuality.vt = VT_R4;

        if ( lowQualityOutput )
            varValueQuality.fltVal = 0.6; // lower quality, smaller
        else
            varValueQuality.fltVal = 1.0; // slower but more compressed

        hr = propertyBag->Write( 1, &optionQuality, &varValueQuality );
        if ( FAILED( hr ) )
        {
            printf( "can't write imagequality to property bag, error %#x\n", hr );
            return hr;
        }
    }

    if ( !wcscmp( outputMimetype, L"image/jpeg" ) )
    {
        PROPBAG2 optionSub = { 0 };
        optionSub.pstrName = L"JpegYCrCbSubsampling";
        VARIANT varValueSub;
        VariantInit( &varValueSub );
        varValueSub.vt = VT_UI1;
        if ( lowQualityOutput )
            varValueSub.iVal = WICJpegYCrCbSubsampling422;  // throw away color data to make it smaller
        else
            varValueSub.iVal = WICJpegYCrCbSubsampling444; // preserve color data

        hr = propertyBag->Write( 1, &optionSub, &varValueSub );
        if ( FAILED( hr ) )
        {
            printf( "can't write Subsampling to property bag, error %#x\n", hr );
            return hr;
        }
    }

    hr = bitmapFrameEncode->Initialize( propertyBag.Get() );
    if ( FAILED( hr ) )
    {
        printf( "can't initialize frame encode with property bag, error %#x\n", hr );
        return hr;
    }

    return hr;
} //CreateWICEncoder

template <class T> void FloodFill( T * buffer, int width, int height, int fillColor )
{
    // Only written and tested for 24bppBGR and 48bppBGR

    int bitsShiftLeft = 8 * ( sizeof T - 1 );
    T fillRed = ( ( fillColor & 0xff0000 ) >> 16 ) << bitsShiftLeft;
    T fillGreen = ( ( fillColor & 0xff00 ) >> 8 ) << bitsShiftLeft;
    T fillBlue = ( fillColor & 0xff ) << bitsShiftLeft;

    int strideInT = StrideInBytes( width, 3 * 8 * sizeof T ) / sizeof T;

    for ( int y = 0; y < height; y++ )
    {
        T * p = ( buffer + y * strideInT );
        for ( int x = 0; x < width; x++ )
        {
            *p++ = fillBlue;
            *p++ = fillGreen;
            *p++ = fillRed;
        }
    }
} //FloodFill

template <class T> __forceinline T MakeGreyscale( T r, T g, T b )
{
    // for 24bpp it's 0 and 8. For 48bpp it's 8 and 16;

    const int lshift = 8 * ( sizeof( T ) - 1 );
    const int rshift = 8 * ( sizeof( T ) );

    return (T) ( ( (int) r * ( 54 << lshift ) + (int) g * ( 182 << lshift ) + (int) b * ( 18 << lshift ) ) >> rshift );
    // very slow: return (T) round( 0.2125 * (double) r + 0.7154 * (double) g + 0.0721 * (double) b );
} //MakeGreyscale

template <class T> void CopyPixels( int start, int beyond, int width, T * pOutBase, T * pInBase, int strideOut, int strideIn, bool makeGreyscale )
{
    byte * pbOut = (byte *) pOutBase + ( strideOut * start );
    byte * pbIn = (byte *) pInBase + ( strideIn * start );
    const int bytesWide = 3 * width * ( sizeof T );
    
    for ( int y = start; y < beyond; y++ )
    {
        if ( makeGreyscale )
        {
            T * pRowIn = (T *) pbIn;
            T * pRowOut = (T *) pbOut;
    
            for ( int x = 0; x < width; x++ )
            {
                T b = *pRowIn++; T g = *pRowIn++; T r = *pRowIn++;
                T grey = MakeGreyscale( r, g, b );
                *pRowOut++ = grey; *pRowOut++ = grey; *pRowOut++ = grey;
            }
        }
        else
        {
            memcpy( pbOut, pbIn, bytesWide );
        }

        pbOut += strideOut;
        pbIn += strideIn;
    }
} //CopyPixels

ULONG CountPixelsOn( BYTE * image, int width, int height, int stride )
{
    ULONG pixelsOn = 0;
    byte * pb = image;

    for ( int y = 0; y < height; y++ )
    {
        byte * p = pb;

        for ( int x = 0; x < width; x++ )
        {
            if ( 0 != *p )
                pixelsOn++;

            p += 3;
        }

        pb += stride;
    }

    return pixelsOn;
} //CountPixelsOn

void CreateWAVFromImage( int waveMethod, WCHAR const * pwcWAVBase, BYTE * image, int stride, int width, int height, int bpp )
{
    // NOTE: This is still very much in prototype mode. Not sure how it will pan out.
    // NOTE: current code below assumes posterization /p:1 and greyscale /g have been set.

    if ( waveMethod > 4 || 24 != bpp )
        return;

    const WORD bytesPerSample = sizeof( short );
    const int sampleRate = 88200;
    DjlParseWav::WavSubchunk fmtOut;
    WCHAR awcWAV[ MAX_PATH ];
    wcscpy( awcWAV, pwcWAVBase );
    wcscat( awcWAV, L".wav" );
    unique_ptr<short> wav;
    int samples;
    
    if ( 1 == waveMethod || 2 == waveMethod )
    {
        // stretch the image as much as needed to make a wav file that contains all on pixels

        bool allPixels = ( 1 == waveMethod) ? false : true;  // true == airplane?  false == fart?
        samples = allPixels ? width * height : CountPixelsOn( image, width, height, stride );
        wav.reset( new short[ samples ] );
        ZeroMemory( wav.get(), samples * sizeof( short ) );
        int cur = 0;
    
        for ( int x = 0; x < width; x++ )
        {
            for ( int y = 0; y < height; y++ )
            {
                byte * pb = image + ( y * stride ) + ( 3 * x );
                if ( *pb )
                {
                    // put in the range of -1..1
                    double yv = ( 2.0 * ( (double) y / (double) height ) - 1.0 );
    
                    short v = (short) ( round ( -yv * 32767.0 ) );
    
                    wav.get()[ cur++ ] = v;
                }
                else if ( allPixels )
                    wav.get()[ cur++ ] = -32768;
            }
        }
    }
    else if ( 3 == waveMethod || 4 == waveMethod )
    {
        byte *pb = image;
        unique_ptr<short> maxY( new short[ width ] );
        ZeroMemory( maxY.get(), width * sizeof( short ) );

        if ( 3 == waveMethod )
        {
            // for each X, only set the vertically highest Y. If no Y are set, set the midpoint. Flip on even/odd X for highest/lowest Y
    
            for ( int x = 0; x < width; x++ )
            {
                bool anyY = false;
    
                if ( true ) //x & 1 )
                {
                    for ( int y = 0; y < height; y++ )
                    {
                        byte * p = pb + ( y * stride ) + ( x * 3 );
         
                        if ( ! anyY )
                        {
                            if ( *p )
                            {
                                anyY = true;
                                maxY.get()[ x ] = (short) ( (double) ( height - y ) / (double) height * 32767 );
                            }
                        }
                        else
                        {
                            *p++ = 0; *p++ = 0; *p = 0;
                        }
                    }
                }
                else
                {
                    for ( int y = height - 1; y >= 0; y-- )
                    {
                        byte * p = pb + ( y * stride ) + ( x * 3 );
        
                        if ( ! anyY )
                        {
                            if ( *p )
                            {
                                anyY = true;
                                maxY.get()[ x ] = (short) ( (double) ( height - y ) / (double) height * 32767 );
                            }
                        }
                        else
                        {
                            *p++ = 0; *p++ = 0; *p = 0;
                        }
                    }
                }
    
                if ( !anyY )
                {
                    byte * p = pb + ( ( height / 2 ) * stride ) + ( x * 3 );
                    *p++ = 255; *p++ = 255; *p = 255;
                    maxY.get()[ x ] = height / 2;
                }
            }
        }
        else
        {
            assert( 4 == waveMethod );
            // Find the Y offset of the brightest Y for each X
    
            for ( int x = 0; x < width; x++ )
            {
                byte brightestY = 0;
                for ( int y = 0; y < height; y++ )
                {
                    byte * p = pb + ( y * stride ) + ( x * 3 );
                    if ( *p > brightestY )
                    {
                        brightestY = *p;
                        maxY.get()[ x ] = (short) ( (double) ( height - y ) / (double) height * 32767 );
                    }
                }
    
                // if none set, use the midpoint
    
                if ( 0 == brightestY )
                {
                    byte * p = pb + ( ( height / 2 ) * stride ) + ( x * 3 );
                    *p++ = 255; *p++ = 255; *p = 255;
                    maxY.get()[ x ] = height / 2;
                }
            }
        }

        // The image is now the first half of the waveform. Invert it to make the second half.
        // Target A above middle C -- 440 Hz at 88200 samples per second, for one second of sound

        samples = 88200;
        int samplesPerWave = (int) round( (double) 1.0 / (double) 440.0 * (double) samples );
        int halfSamplesPerWave = samplesPerWave / 2;
        printf( "width %d, samplesPerWave %d, halfSamplesPerWave %d\n", width, samplesPerWave, halfSamplesPerWave );

        wav.reset( new short[ samples ] );
        ZeroMemory( wav.get(), samples * sizeof( short ) );

        for ( int t = 0; t < halfSamplesPerWave; t++ )
        {
            int x = (int) round( (double) t / (double) halfSamplesPerWave * (double) width );
            wav.get()[ t ] = maxY.get()[ x ];
            wav.get()[ t + halfSamplesPerWave ] = - maxY.get()[x];
        }

        // replicate the wave throughout the buffer

        int copies = samples / samplesPerWave;
        for ( int c = 1; c < copies; c++ )
            memcpy( wav.get() + c * samplesPerWave, wav.get(), samplesPerWave * sizeof( short ) );
    }

    // write the WAV to disk

    DjlParseWav::InitializeWavSubchunk( fmtOut, 1, 1, sampleRate, bytesPerSample, 8 * bytesPerSample );
    DjlParseWav output( awcWAV, fmtOut );
    if ( !output.OpenSuccessful() )
    {
        printf( "can't open WAV output file %ws\n", awcWAV );
        return;
    }
    
    bool ok = output.WriteWavFile( (byte *) wav.get(), samples * sizeof( short ) );
    if ( !ok )
        printf( "can't write WAV file %ws\n", awcWAV );
    else
        printf( "created WAV file: %ws\n", awcWAV );
} //CreateWAVFromImage

template <class T> __forceinline T Posterize( T * pt, int groupSpan, int groupAmount )
{
    // posterization level 1 means 2 values: 0 and 255
    //                     2       3       : 0, 127, 255
    //                     3       4       : 0, 85, 170, 255

    return ( *pt / groupSpan ) * groupAmount;
} //Posterize

template <class T> void PosterizeImage( T * image, int stride, int width, int height, int posterizeLevel )
{
    assert( 0 != posterizeLevel );
    T * row = image;
    const int maxT = ( 1 << sizeof( T ) * 8 ) - 1;
    const int values = posterizeLevel + 1;
    const int groupSpan = maxT / values;
    const int groupAmount = maxT / posterizeLevel;

    for ( int y = 0; y < height; y++ )
    {
        T * pRow = row;
    
        for ( int x = 0; x < width; x++ )
        {
            *pRow++ = Posterize( pRow, groupSpan, groupAmount );
            *pRow++ = Posterize( pRow, groupSpan, groupAmount );
            *pRow++ = Posterize( pRow, groupSpan, groupAmount );
        }

        row += ( stride * sizeof T );
    }
} //PosterizeImage

// Note: this is effectively a blt -- there is no stretching or scaling.

HRESULT DrawImage( byte * pOut, int strideOut, ComPtr<IWICBitmapSource> & source, int waveMethod, const WCHAR * pwcWAVBase,
                   int posterizeLevel, bool makeGreyscale, int offsetX, int offsetY, int width, int height, int bppIn, int bppOut )
{
    int strideIn = StrideInBytes( width, bppIn );

    tracer.Trace( "DrawImage: strideIn %d, strideOut %d, offsetX %d, offsetY %d, width %d, height %d, bppIn %d, bppOut %d\n",
                  strideIn, strideOut, offsetX, offsetY, width, height, bppIn, bppOut );

    // this function only supports 24BGR to 24BGR or 48RGB to 48RGB

    if ( bppIn != bppOut || ( bppIn != 24 && bppIn != 48 ) )
    {
        printf( "bppIn %d doesn't match bppOut %d\n", bppIn, bppOut );
        return E_FAIL;
    }

    int cbIn = strideIn * height;
    unique_ptr<::byte> bufferIn( new ::byte[ cbIn ] );
    
    {
        CTimed timed( g_CollageStitchCopyPixelsTime );

        // Almost all of the runtime of this app will be in source->CopyPixels(), where the input file is parsed

        HRESULT hr = source->CopyPixels( 0, strideIn, cbIn, bufferIn.get() );
        if ( FAILED( hr ) )
        {
            printf( "DrawImage() failed to read input pixels in CopyPixels() %#x\n", hr );
            return hr;
        }
    }

    CTimed stitchDraw( g_CollageStitchDrawTime );
    int bytesppOut = bppOut / 8;
    byte * pbOutBase = pOut + ( offsetY * strideOut ) + ( offsetX * bytesppOut );
    byte * pbInBase = bufferIn.get();

    if ( 24 == bppIn )
        CopyPixels( 0, height, width, pbOutBase, pbInBase, strideOut, strideIn, makeGreyscale );
    else
        CopyPixels( 0, height, width, (USHORT *) pbOutBase, (USHORT *) pbInBase, strideOut, strideIn, makeGreyscale );

    if ( 0 != posterizeLevel )
    {
        if ( 24 == bppOut )
            PosterizeImage( pOut, strideOut, width, height, posterizeLevel );
        else
            PosterizeImage( (USHORT *) pOut, strideOut, width, height, posterizeLevel );
    }

    if ( 0 != waveMethod )
       CreateWAVFromImage( waveMethod, pwcWAVBase, pOut, strideOut, width, height, bppOut );

    return S_OK;
} //DrawImage

HRESULT CommitEncoder( ComPtr<IWICBitmapFrameEncode> & bitmapFrameEncode, ComPtr<IWICBitmapEncoder> & encoder )
{
    HRESULT hr = bitmapFrameEncode->Commit();
    if ( FAILED( hr ) )
    {
        printf( "can't commit the frame encoder, error %#x\n", hr );
        return hr;
    }

    hr = encoder->Commit();
    if ( FAILED( hr ) )
    {
        printf( "can't commit the encoder, error %#x\n", hr );
        return hr;
    }

    return hr;
} //CommitEncoder

HRESULT WriteWICBitmap( WCHAR const * pwcOutput, ComPtr<IWICBitmapSource> & source, ComPtr<IWICBitmapFrameDecode> & frame,
                        int longEdge, int waveMethod, int posterizeLevel, bool makeGreyscale, double aspectRatio, int fillColor,
                        WCHAR const * outputMimetype, bool lowQualityOutput )
{
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> bitmapFrameEncode;
    HRESULT hr = CreateWICEncoder( pwcOutput, encoder, bitmapFrameEncode, outputMimetype, lowQualityOutput );
    if ( FAILED( hr ) )
    {
        printf( "can't create a wic encoder, error %#x\n", hr );
        return hr;
    }

#if false // this fails in WIC when copying from TIF to JPG because the layout isn't indentical
    hr = CopyMetadata( bitmapFrameEncode, frame );
    if ( FAILED( hr ) )
    {
        printf( "can't copy image metadata, error %#x\n", hr );
        return hr;
    }
#endif

    UINT width, height;
    hr = source->GetSize( &width, &height );
    if ( FAILED( hr ) )
    {
        printf( "can't get the dimensions of the source bitmap: %#x\n", hr );
        return hr;
    }

    tracer.Trace( "source bitmap width %d, height %d\n", width, height );

    double originalAspectRatio = (double) width / (double) height;

    if ( ( ( 0.0 == aspectRatio ) || SameDouble( aspectRatio, originalAspectRatio ) ) && ( 0 == posterizeLevel ) && ( 0 == waveMethod ) )
    {
        // This codepath is just an optimization for when it can be used. 

        if ( makeGreyscale )
        {
            // it'll be written out as 1 8-bit channel to the JPG, but in the meantime...
    
            GUID pixelFormat = GUID_WICPixelFormat32bppGrayFloat;
            hr = bitmapFrameEncode->SetPixelFormat( & pixelFormat  );
            if ( FAILED( hr ) )
            {
                printf( "can't make pixel format greyscale, error %#x\n", hr );
                return hr;
            }
        }

        if ( 0 != longEdge )
        {
            hr = ScaleWICBitmap( source, longEdge );
            if ( FAILED( hr ) )
            {
                printf( "failed to scale input bitmap %#x\n", hr );
                return hr;
            }
        }

        hr = bitmapFrameEncode->WriteSource( source.Get(), NULL );
        if ( FAILED( hr ) )
        {
            if ( MF_E_INVALID_FORMAT == hr )
                printf( "  MF_E_INVALID_FORMAT:  " );
            printf( "can't write bitmap to encoder, error %#x\n", hr );
            return hr;
        }
    }
    else
    {
        // fill the bitmap with the fill color then center the source in the output

        UINT offsetX = 0;
        UINT offsetY = 0;
        UINT wOut = width;
        UINT hOut = height;
        UINT wIn = width;
        UINT hIn = height;

        if ( 0.0 == aspectRatio )
            aspectRatio = originalAspectRatio;

        if ( 0 != longEdge )
        {
            // Don't allow scaling to a resolution larger than the original

            if ( longEdge > width && longEdge > height )
                longEdge = __max( width, height );

            UINT wScaled = 0;
            UINT hScaled = 0;

            if ( aspectRatio > originalAspectRatio )
            {
                wOut = longEdge;
                hOut = (UINT) round( (double) longEdge / aspectRatio );

                hIn = hOut;
                wIn = (UINT) round( (double) width * (double) hIn / (double) height );
                offsetX = ( wOut - wIn ) / 2;
            }
            else
            {
                hOut = longEdge;
                wOut = (UINT) round( (double) longEdge * aspectRatio );

                wIn = wOut;
                hIn = (UINT) round( (double) height * (double) wIn / (double) width );
                offsetY = ( hOut - hIn ) / 2;
            }
        }
        else
        {
            if ( SameDouble( aspectRatio, 1.0f ) )
            {
                hOut = __max( height, width );
                wOut = hOut;

                if ( width > height )
                    offsetY = ( hOut - height ) / 2;
                else
                    offsetX = ( wOut - width ) / 2;
            }
            else if ( aspectRatio > originalAspectRatio )
            {
                wOut = (UINT) round( (double) height * aspectRatio );
                offsetX = ( wOut - width ) / 2;
            }
            else
            {
                hOut = (UINT) round( ( (double) width / aspectRatio ) );
                offsetY = ( hOut - height ) / 2;
            }
        }

        tracer.Trace( "WriteWICBitmap: offsetx %d, offsety %d, wIn %d, hIn %d, wOut %d, hOut %d\n", offsetX, offsetY, wIn, hIn, wOut, hOut );

        hr = ScaleWICBitmap( source, wIn > hIn ? wIn : hIn );
        if ( FAILED( hr ) )
        {
            printf( "failed to scale input bitmap %#x\n", hr );
            return hr;
        }

        hr = bitmapFrameEncode->SetSize( wOut, hOut );
        if ( FAILED( hr ) )
        {
            printf( "failed to set encoder frame size %d, %d, error %#x\n", wOut, hOut, hr );
            return hr;
        }

        WICPixelFormatGUID formatSource;
        hr = source->GetPixelFormat( &formatSource );
        if ( FAILED( hr ) )
        {
            printf( "can't get source pixelFormat: %#x\n", hr );
            return hr;
        }

        // try for output of 24bppBGR unless input is 48bppRGB and output is tiff

        WICPixelFormatGUID formatRequest = g_GuidPixelFormat;

        if ( !wcscmp( outputMimetype, L"image/tiff" ) )
            formatRequest = formatSource;

        hr = bitmapFrameEncode->SetPixelFormat( & formatRequest );
        if ( FAILED( hr ) )
        {
            printf( "failed to set encoder pixel format %#x\n", hr );
            return hr;
        }

        if ( formatRequest != formatSource )
        {
            printf( "didn't get the pixel format requested for writing\n" );
            return E_FAIL;
        }

        int bppOut = g_BitsPerPixel;
        if ( GUID_WICPixelFormat48bppRGB == formatRequest )
            bppOut = 48;

        int bppIn = g_BitsPerPixel;
        if ( GUID_WICPixelFormat48bppRGB == formatSource )
            bppIn = 48;

        tracer.Trace( "bppIn %d, bppOut %d\n", bppIn, bppOut );

        int strideOut = StrideInBytes( wOut, bppOut );
        int cbOut = strideOut * hOut;
        unique_ptr<::byte> bufferOut( new ::byte[ cbOut ] );

        if ( 24 == bppOut )
            FloodFill( bufferOut.get(), wOut, hOut, fillColor );
        else
            FloodFill( (USHORT *) bufferOut.get(), wOut, hOut, fillColor );

        hr = DrawImage( bufferOut.get(), strideOut, source, waveMethod, pwcOutput, posterizeLevel, makeGreyscale,
                        offsetX, offsetY, wIn, hIn, bppIn, bppOut );
        if ( FAILED( hr ) )
        {
            printf( "failed to DrawImage %#x\n", hr );
            return hr;
        }

        hr = bitmapFrameEncode->WritePixels( hOut, strideOut, cbOut, bufferOut.get() );
        if ( FAILED( hr ) )
        {
            printf( "failed to write pixels %#x\n", hr );
            return hr;
        }
    }

    hr = CommitEncoder( bitmapFrameEncode, encoder );

    return hr;
} //WriteWICBitmap

HRESULT GetBitmapDimensions( WCHAR const * path, UINT & width, UINT & height )
{
    width = 0;
    height = 0;

    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICBitmapFrameDecode> frame;

    HRESULT hr = LoadWICBitmap( path, source, frame, false );
    if ( FAILED( hr ) )
    {
        printf( "can't open bitmap %ws\n", path );
        return hr;
    }

    hr = source->GetSize( &width, &height );
    if ( FAILED( hr ) )
        printf( "can't get dimensions of path %ws\n", path );

    return hr;
} //GetBitmapDimensions

HRESULT StitchImages( WCHAR const * pwcOutput, CPathArray & pathArray, vector<BitmapDimensions> & dimensions, int imagesWide, int imagesHigh,
                      int cellDX, int cellDY, int stitchDX, int stitchDY, int fillColor, int waveMethod, int posterizeLevel, bool makeGreyscale,
                      WCHAR const * outputMimetype, bool lowQualityOutput )
{
    CTimed timeStitch( g_CollageStitchTime );
    bool makeEverythingSquare = ( cellDX == cellDY );
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> bitmapFrameEncode;
    HRESULT hr = CreateWICEncoder( pwcOutput, encoder, bitmapFrameEncode, outputMimetype, lowQualityOutput );
    if ( FAILED( hr ) )
    {
        printf( "can't create a wic encoder: error %#x\n", hr );
        return hr;
    }
    
    hr = bitmapFrameEncode->SetSize( stitchDX, stitchDY );
    if ( FAILED( hr ) )
    {
        printf( "failed to set encoder frame size %#x\n", hr );
        return hr;
    }
    
    WICPixelFormatGUID formatRequest = g_GuidPixelFormat;
    hr = bitmapFrameEncode->SetPixelFormat( & formatRequest );
    if ( FAILED( hr ) )
    {
        printf( "failed to set encoder pixel format %#x\n", hr );
        return hr;
    }
    
    if ( formatRequest != g_GuidPixelFormat )
    {
        printf( "didn't get the pixel format requested for writing\n" );
        return E_FAIL;
    }
    
    int strideOut = StrideInBytes( stitchDX, g_BitsPerPixel );
    int cbOut = strideOut * stitchDY;
    unique_ptr<::byte> bufferOut( new ::byte[ cbOut ] );
    
    {
        CTimed timedFlood( g_CollageStitchFloodTime );
    
        FloodFill( bufferOut.get(), stitchDX, stitchDY, fillColor );
    }
    
    //for ( int y = 0; y < imagesHigh; y++ )
    parallel_for( 0, imagesHigh, [&] ( int y )
    {
        int yOffset = y * cellDY;
    
        //for ( int x = 0; x < imagesWide; x++ )
        parallel_for( 0, imagesWide, [&] ( int x )
        {
            int xOffset = x * cellDX;
            int curSource = y * imagesWide + x;
    
            if ( curSource < pathArray.Count() )
            {
                ComPtr<IWICBitmapSource> source;
                ComPtr<IWICBitmapFrameDecode> frame;
                HRESULT hr = LoadWICBitmap( pathArray[curSource].pwcPath, source, frame, true );
                if ( FAILED( hr ) )
                    printf( "can't open bitmap, error: %#x\n", hr );
    
                if ( SUCCEEDED( hr ) )
                {
                    hr = ScaleWICBitmap( source, __max( cellDY, cellDX ) );
                    if ( FAILED( hr ) )
                        printf( "can't scale source bitmap, error %#x\n", hr );
                }
    
                UINT width = 0, height = 0;
    
                if ( SUCCEEDED( hr ) )
                {
                    hr = source->GetSize( &width, &height );
                    if ( FAILED( hr ) )
                        printf( "can't get dimensions of path error %#x\n", hr );
                }
    
                if ( SUCCEEDED( hr ) )
                {
                    int rectX = xOffset;
                    int rectY = yOffset;
    
                    if ( makeEverythingSquare && ( width != height ) )
                    {
                        if ( width > height )
                        {
                            double diff = width - height;
                            double resultDiff = round( diff * (double) cellDY / (double) width );
                            rectY += (int) round( resultDiff / 2.0 );
                        }
                        else
                        {
                            double diff = height - width;
                            double resultDiff = round( diff * (double) cellDX / (double) height );
                            rectX += (int) round( resultDiff / 2.0 );
                        }
                    }
    
                    DrawImage( bufferOut.get(), strideOut, source, waveMethod, pwcOutput, posterizeLevel, makeGreyscale,
                               rectX, rectY, width, height, g_BitsPerPixel, g_BitsPerPixel );
                }
            }
        });
    });
    
    timeStitch.Complete();

    // If the output image is large, most of the time in the app is spent here compressing and writing the image

    CTimed timeWrite( g_CollageWriteTime );

    hr = bitmapFrameEncode->WritePixels( stitchDY, strideOut, cbOut, bufferOut.get() );
    if ( FAILED( hr ) )
    {
        printf( "failed to write pixels %#x\n", hr );
        return hr;
    }

    hr = CommitEncoder( bitmapFrameEncode, encoder );

    return hr;
} //StitchImages

HRESULT GenerateCollage( WCHAR * pwcInput, const WCHAR * pwcOutput, int longEdge, int posterizeLevel, bool makeGreyscale,
                         double aspectRatio, int fillColor, WCHAR const * outputMimetype, bool randomizeCollage,
                         bool lowQualityOutput )
{
    CTimed timePrep( g_CollagePrepTime );

    if ( 0.0 == aspectRatio )
        aspectRatio = 1.0;

    WCHAR awcPath[ MAX_PATH + 1 ];
    WCHAR awcSpec[ MAX_PATH + 1 ];
    
    WCHAR * pwcSlash = wcsrchr( pwcInput, L'\\' );
    
    if ( NULL == pwcSlash )
    {
        wcscpy( awcSpec, pwcInput );
        _wfullpath( awcPath, L".\\", sizeof awcPath / sizeof WCHAR );
    }
    else
    {
        wcscpy( awcSpec, pwcSlash + 1 );
        *(pwcSlash + 1) = 0;
        _wfullpath( awcPath, pwcInput, sizeof awcPath / sizeof WCHAR );
    }
    
    tracer.Trace( "GenerateCollage: Path '%ws', File Specificaiton '%ws'\n", awcPath, awcSpec );

    CPathArray pathArray;
    CEnumFolder enumPaths( false, &pathArray, NULL, 0 );
    enumPaths.Enumerate( awcPath, awcSpec );

    size_t fileCount = pathArray.Count();
    printf( "files found: %zd\n", fileCount );

    if ( 0 == fileCount )
    {
        printf( "no files found in path %ws and pattern %ws\n", awcPath, awcSpec );
        return E_FAIL;
    }

    if ( randomizeCollage )
        pathArray.Randomize();

    vector<BitmapDimensions> dimensions( fileCount );
    HRESULT hr = S_OK;

    //for ( size_t i = 0; i < fileCount; i++ )
    parallel_for( (size_t) 0, fileCount, [&] ( size_t i )
    {
        HRESULT hrDim = GetBitmapDimensions( pathArray[ i ].pwcPath, dimensions[ i ].width, dimensions[ i ].height );

        if ( FAILED( hrDim ) )
            hr = hrDim;
    });

    if ( FAILED( hr ) )
    {
        printf( "loading dimensions failed for one or more images\n" );
        return hr;
    }

    int minEdge = 1000000000;
    int minDXEdge = minEdge;
    int minDYEdge = minEdge;
    vector<UINT> longEdges( fileCount );

    for ( size_t i = 0; i < fileCount; i++ )
    {
        longEdges[i] = __max( dimensions[i].height, dimensions[i].width );

        if ( longEdges[i] < minEdge )
            minEdge = longEdges[i];

        if ( dimensions[i].width < minDXEdge )
            minDXEdge = dimensions[i].width;

        if ( dimensions[i].height < minDYEdge )
            minDYEdge = dimensions[i].height;
    }

    if ( 0 == minEdge )
    {
        printf( "need a non-0-sized bitmap" );
        return E_FAIL;
    }

    bool allSameAspect = true;

    // Check if the aspect ratio of all images is roughly the same

    double aspect = 0.0;
    for ( size_t i = 0; i < fileCount; i++ )
    {
        double a = (double) dimensions[i].width / (double) dimensions[i].height;

        if ( SameDouble( aspect, 0.0 ) )
            aspect = a;
        else
        {
            if ( !SameDouble( a, aspect ) )
            {
                allSameAspect = false;
                break;
            }
        }
    }

    tracer.Trace( "GenerateCollage: minEdge %d minDXEdge %d, min DYEdge %d, all same aspect: %d\n", minEdge, minDXEdge, minDYEdge, allSameAspect );

    // Try each permutation of stacking the images to find the one that most conforms to the target aspect ratio.

    int imagesWide = 0;
    int imagesHigh = 0;
    double bestAspect = 100000.0;
    double desiredAspect = aspectRatio; // count across / count up and down

    for ( size_t x = 1; x <= fileCount; x++ )
    {
        for ( int y = 1; y <= fileCount; y++ )
        {
            int capacity = x * y;
            if ( capacity < fileCount )
                continue;

            int unused = capacity - fileCount;
            if ( unused >= x || unused >= y )
                continue;

            double testAspect = 0;

            if ( allSameAspect )
                testAspect = ( (double) x * (double) minDXEdge ) / ( (double) y * (double) minDYEdge );
            else
                testAspect = ( (double) x * (double) minEdge ) / ( (double) y * (double) minEdge );

            double distance = fabs( desiredAspect - testAspect );
            if ( distance < bestAspect )
            {
                bestAspect = distance;
                imagesWide = x;
                imagesHigh = y;
            }
        }
    }

    int stitchX = imagesWide * minEdge;
    int stitchY = imagesHigh * minEdge;

    if ( allSameAspect )
    {
        stitchX = imagesWide * minDXEdge;
        stitchY = imagesHigh * minDYEdge;
    }

    // 2 billion is probably enough?
    int maxLongestDimension = 2000000000;

    if ( 0 != longEdge )
        maxLongestDimension = longEdge;

    // Note: the logic below may result in a dimension and/or aspect ratio that isn't exactly as requested
    // by the caller since the dimensions must be a multiple of the individual image sizes.

    double dmaxLongestDimension = (double) maxLongestDimension;

    if ( stitchX > maxLongestDimension || stitchY > maxLongestDimension )
    {
        if ( allSameAspect )
        {
            if ( minDYEdge > minDXEdge )
            {
                double dScale = ( dmaxLongestDimension / (double) imagesHigh ) / (double) minDYEdge;
                minDXEdge = (int) round( dScale * (double) minDXEdge );
                minDYEdge = maxLongestDimension / imagesHigh;
                stitchY = maxLongestDimension;
                stitchX = imagesWide * minDXEdge;
            }
            else
            {
                double dScale = ( dmaxLongestDimension / (double) imagesWide ) / (double) minDXEdge;
                minDYEdge = (int) round( dScale * (double) minDYEdge );
                minDXEdge = maxLongestDimension / imagesWide;
                stitchX = maxLongestDimension;
                stitchY = imagesHigh * minDYEdge;
            }
        }
        else
        {
            if ( stitchX > stitchY )
                minEdge = maxLongestDimension / imagesWide;
            else
                minEdge = maxLongestDimension / imagesHigh;

            stitchX = minEdge * imagesWide;
            stitchY = minEdge * imagesHigh;
        }
    }

    if ( !allSameAspect )
    {
        minDXEdge = minEdge;
        minDYEdge = minEdge;
    }

    timePrep.Complete();

    printf( "source images%s all have the same aspect ratio\n", allSameAspect ? "" : " don't" );
    printf( "collage will be %d by %d, each element %d by %d, and %d by %d images\n", stitchX, stitchY, minDXEdge, minDYEdge, imagesWide, imagesHigh );

    return StitchImages( pwcOutput, pathArray, dimensions, imagesWide, imagesHigh, minDXEdge, minDYEdge, stitchX, stitchY,
                         fillColor, 0, posterizeLevel, makeGreyscale, outputMimetype, lowQualityOutput );
} //GenerateCollage

HRESULT ConvertImage( WCHAR const * input, WCHAR const * output, int longEdge, int waveMethod, int posterizeLevel, bool makeGreyscale,
                      double aspectRatio, int fillColor, WCHAR const * outputMimetype, bool lowQualityOutput )
{
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICBitmapFrameDecode> frame;
    bool force24bppBGR = wcscmp( outputMimetype, L"image/tiff" );

    HRESULT hr = LoadWICBitmap( input, source, frame, force24bppBGR );
    if ( SUCCEEDED( hr ) )
        hr = WriteWICBitmap( output, source, frame, longEdge, waveMethod, posterizeLevel, makeGreyscale, aspectRatio, fillColor, outputMimetype, lowQualityOutput );
    
    frame.Reset();
    source.Reset();
    return hr;
} //ConvertImage

void Usage()
{
    printf( "usage: ic <input> /o:<filename>\n" );
    printf( "  Image Convert\n" );
    printf( "  arguments: <input>           The input image filename or path specifier for a collage.\n" );
    printf( "             -a:<aspectratio>  Aspect ratio of output (widthXheight) (e.g. 3x2, 3x4, 16x9, 1x1, 8.51x3.14). Default 1x1 for collages.\n" );
    printf( "             -c                Generate a collage. <input> is a path to input files. Aspect ratio attempted, not guaranteed.\n" );
    printf( "             -f:<fillcolor>    Color fill for empty space. ARGB or RGB in hex. Default is black.\n" );
    printf( "             -g                Greyscale the output image. Does not apply to the fillcolor.\n" );
    printf( "             -i                Show CPU and RAM usage.\n" );
    printf( "             -l:<longedge>     Pixel count for the long edge of the output photo.\n" );
    printf( "             -o:<filename>     The output filename. Required argument. File will contain no exif info like GPS location.\n" );
    printf( "             -p:x              Posterization level. 0..35 inclusive, where 0 means none. Default is 0.\n" );
    printf( "             -q                Sacrifice image quality to produce a smaller JPG output file (4:2:2 not 4:4:4, 60%% not 100%%).\n" );
    printf( "             -r                Randomize the layout of images in a collage.\n" );
    printf( "             -t                Enable debug tracing to ic.txt. Use -T to start with a fresh ic.txt\n" );
    printf( "             -w:x              Create a WAV file based on the image using methods 1..10. (prototype)\n" );
    printf( "  sample usage: (arguments can use - or /)\n" );
    printf( "    ic picture.jpg /o:newpicture.jpg /l:800\n" );
    printf( "    ic picture.jpg /p o:newpicture.jpg /l:800\n" );
    printf( "    ic picture.jpg /o:c:\\folder\\newpicture.jpg /l:800 /a:1x1\n" );
    printf( "    ic tsuki.tif /o:newpicture.jpg /l:300 /a:1x4 /f:0x003300\n" );
    printf( "    ic miku.tif /o:newpicture.tif /l:300 /a:1x4 /f:2211\n" );
    printf( "    ic phoebe.jpg /o:phoebe_grey.tif /l:3000 /g\n" );
    printf( "    ic julien.jpg /o:julien_grey_posterized.tif /l:3000 /g /p:1 /w:1\n" );
    printf( "    ic picture.jpg /o:newpicture.jpg /l:2000 /a:5.2x3.9\n" );
    printf( "    ic *.jpg /c /o:c:\\collage.jpg /l:2000 /a:5x3 /f:0xff00aa88\n" );
    printf( "    ic d:\\pictures\\mitski\\*.jpg /c /o:mitski_collage.jpg /l:10000 /a:4x5\n" );
    printf( "  notes:    - -g only applies to the image, not fillcolor. Use /f with identical rgb values for greyscale fills.\n" );
    printf( "            - Exif data is stripped for your protection.\n" );
    printf( "            - fillcolor may or may not start with 0x.\n" );
    printf( "            - Both -a and -l are aspirational for collages. Aspect ratio and long edge may change to accomodate content.\n" );
    printf( "            - If a precise collage aspect ratio or long edge are required, run the app twice; on a single image it's exact.\n" );
    printf( "            - Writes as high a quality of JPG as it can: 1.0 quality and 4:4:4\n" );
    printf( "            - <input> can be any WIC-compatible format: heic, tif, png, bmp, cr2, jpg, etc.\n" );
    printf( "            - Output file is always 24bpp unless both input and output are tif and input is 48bpp, which results in 48bpp.\n" );
    printf( "            - Output file type is inferred from the extension specified. JPG is assumed if not obvious.\n" );
    exit( 0 );
} //Usage

double ParseAspectRatio( WCHAR const * p )
{
    WCHAR const * px = wcschr( p, L'x' );
    if ( ( 0 == px ) || ( wcslen( p ) > 20 ) )
    {
        printf( "aspect ratio specification is invalid: %ws\n", p );
        Usage();
    }

    WCHAR awc[ 21 ];
    wcscpy( awc, p );
    awc[ px - p ] = 0;

    double w = _wtof( awc );
    double h = _wtof( awc + ( px - p ) + 1 );

    if ( w <= 0.0 || h <= 0.0 )
    {
        printf( "aspect ratio %lf by %lf is invalid\n", w, h );
        Usage();
    }

    tracer.Trace( "aspect ratio %ws: w %lf, h %lf, a %lf\n", p, w, h, w / h );

    return w / h;
} //ParseAspectRatio

void AppendLongLong( char *pc, long long n )
{
    if ( n < 0 )
    {
        *pc++ = '-';
        *pc = 0;
        AppendLongLong( pc, -n );
    }

    if ( n < 1000 )
    {
        sprintf( pc, "%lld", n );
        return;
    }

    AppendLongLong( pc, n / 1000 );

    pc += strlen( pc );
    sprintf( pc, ",%03lld", n % 1000 );
} //AppendLongLong

void PrintStat( char const * pcName, long long n, int widthName = 20, int widthN = 13 )
{
    // LLONG_MAX is 9,223,372,036,854,775,807. Add a negative sign and that's 26 characters.

    char acNum[ 40 ];
    acNum[0] = 0;
    AppendLongLong( acNum, n );
    printf( "%-*s %*s\n", widthName, pcName, widthN, acNum );
} //PrintStat

const WCHAR * InferOutputType( WCHAR *ext )
{
    if ( !wcsicmp( ext, L".bmp" ) )
        return L"image/bmp";

    if ( !wcsicmp( ext, L".gif" ) )
        return L"image/gif";

    if ( ( !wcsicmp( ext, L".jpg" ) ) || ( !wcsicmp( ext, L".jpeg" ) ) )
        return L"image/jpeg";

    if ( !wcsicmp( ext, L".png" ) )
        return L"image/png";

    if ( ( !wcsicmp( ext, L".tif" ) ) || ( !wcsicmp( ext, L".tiff" ) ) )
        return L"image/tiff";

    return L"image/jpeg";
} //InferOutputType

WCHAR awcInput[ MAX_PATH ];
WCHAR awcOutput[ MAX_PATH ];

extern "C" int wmain( int argc, WCHAR * argv[] )
{
    long long totalTime = 0;
    CTimed timedTotal( totalTime );

    if ( argc < 2 )
        Usage();

    awcInput[0] = 0;
    awcOutput[0] = 0;
    bool generateCollage = false;
    bool randomizeCollage = false;
    bool runtimeInfo = false;
    bool makeGreyscale = false;
    bool lowQualityOutput = false;
    int posterizeLevel = 0;  // 0 means none
    int waveMethod = 0;      // 0 means none; don't create a WAV file
    int longEdge = 0;
    int fillColor = 0xff << 24; // black, non-transparent
    double aspectRatio = 0.0;
    int breakTileSize = 0; // if 0, don't do it.
    double tileProbability = 1.0;
    bool enableTracing = false;
    bool clearTraceFile = false;

    for ( int a = 1; a < argc; a++ )
    {
        WCHAR const * parg = argv[ a ];

        if ( L'-' == parg[0] || L'/' == parg[0] )
        {
            WCHAR p = tolower( parg[1] );

            if ( L'a' == p )
            {
                if ( L':' != parg[2] )
                    Usage();

                aspectRatio = ParseAspectRatio( parg + 3 );
            }
            else if ( L'c' == p )
                generateCollage = true;
            else if ( L'f' == p )
            {
                if ( L':' != parg[2] )
                    Usage();

               int parsed = swscanf_s( parg + 3, L"%x", & fillColor );

               if ( 0 == parsed )
               {
                   printf( "can't parse fill color\n" );
                   Usage();
               }
            }
            else if ( L'g' == p )
                makeGreyscale = true;
            else if ( L'i' == p )
                runtimeInfo = true;
            else if ( L'l' == p )
            {
                if ( L':' != parg[2] )
                    Usage();

                longEdge = _wtoi( parg + 3 );

                if ( longEdge <= 0 )
                {
                    printf( "long edge -l is invalid: %d\n", longEdge );
                    Usage();
                }
            }
            else if ( L'o' == p )
            {
                if ( L':' != parg[2] )
                    Usage();

                _wfullpath( awcOutput, parg + 3, _countof( awcOutput ) );
            }
            else if ( L'p' == p )
            {
                if ( L':' != parg[2] )
                    Usage();

                posterizeLevel = _wtoi( parg + 3 );
                if ( posterizeLevel < 0 || posterizeLevel > 35 )
                {
                    printf( "invalid posterization level %d\n", posterizeLevel );
                    Usage();
                }
            }
            else if ( L'q' == p )
                lowQualityOutput = true;
            else if ( L'r' == p )
                randomizeCollage = true;
            else if ( L't' == p )
            {
                if ( 0 != parg[2] )
                    Usage();
                enableTracing = true;
                clearTraceFile = ( L'T' == parg[1] );
            }
            else if ( L'w' == p )
            {
                if ( L':' != parg[2] )
                    Usage();

                waveMethod = _wtoi( parg + 3 );
                if ( waveMethod < 0 || waveMethod > 10 )
                {
                    printf( "invalid wave method %d\n", waveMethod );
                    Usage();
                }
            }
            else
                Usage();
        }
        else if ( 0 != awcInput[0] )
            Usage();
        else
            _wfullpath( awcInput, parg, _countof( awcInput ) );
    }

    tracer.Enable( enableTracing, L"ic.txt", clearTraceFile );

    if ( 0 == awcInput[0] || 0 == awcOutput[0] )
    {
        printf( "input and/or output files not specified\n" );
        Usage();
    }

    if ( waveMethod > 0 && generateCollage )
    {
        printf( "can't generate wav files when generating a collage\n" );
        Usage();
    }

    if ( !generateCollage )
    {
        DWORD attr = GetFileAttributesW( awcInput );
        if ( INVALID_FILE_ATTRIBUTES == attr )
        {
            printf( "can't open file %ws\n", awcInput );
            Usage();
        }
    }

    const WCHAR * outputMimetype = InferOutputType( PathFindExtension( awcOutput ) );

    tracer.Trace( "input: %ws\n", awcInput );
    tracer.Trace( "output: %ws\n", awcOutput );
    tracer.Trace( "output type: %ws\n", outputMimetype );
    tracer.Trace( "long edge: %d\n", longEdge );

    HRESULT hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if ( FAILED( hr ) )
    {
        printf( "can't initialize COM: %#x\n", hr );
        return 0;
    }

    hr = CoCreateInstance( CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                           __uuidof( IWICImagingFactory ),
                           (void **) g_IWICFactory.GetAddressOf() );
    if ( FAILED( hr ) )
    {
        printf( "can't create WIC imaging factory: %#x\n", hr );
    }
    else if ( generateCollage )
    {
        hr = GenerateCollage( awcInput, awcOutput, longEdge, posterizeLevel, makeGreyscale, aspectRatio, fillColor, outputMimetype,
                              randomizeCollage, lowQualityOutput );

        if ( SUCCEEDED( hr ) )
            printf( "collage written successfully: %ws\n", awcOutput );
        else
            DeleteFile( awcOutput );
    }
    else
    {
        hr = ConvertImage( awcInput, awcOutput, longEdge, waveMethod, posterizeLevel, makeGreyscale, aspectRatio, fillColor, outputMimetype, lowQualityOutput );
    
        if ( SUCCEEDED( hr ) )
            printf( "output written successfully: %ws\n", awcOutput );
        else
        {
            printf( "conversion of image failed with error %#x\n", hr );
            DeleteFile( awcOutput );
        }
    }

    g_IWICFactory.Reset();
    CoUninitialize();

    if ( runtimeInfo )
    {
        timedTotal.Complete();

        PROCESS_MEMORY_COUNTERS_EX pmc;
        pmc.cb = sizeof pmc;
        if ( GetProcessMemoryInfo( GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS) &pmc, sizeof PROCESS_MEMORY_COUNTERS_EX ) )
        {
            PrintStat( "peak working set:", pmc.PeakWorkingSetSize );
            PrintStat( "final working set:", pmc.WorkingSetSize );
        }

        if ( generateCollage )
        {
            PrintStat( "collage prep:", g_CollagePrepTime / CTimed::NanoPerMilli() );
            PrintStat( "collage stitch:", g_CollageStitchTime / CTimed::NanoPerMilli() );
            PrintStat( "  flood fill:", g_CollageStitchFloodTime / CTimed::NanoPerMilli() );
            PrintStat( "  copy pixels:", g_CollageStitchCopyPixelsTime / CTimed::NanoPerMilli() );
            PrintStat( "  draw:", g_CollageStitchDrawTime / CTimed::NanoPerMilli() );
            PrintStat( "collage write:", g_CollageWriteTime / CTimed::NanoPerMilli() );
        }

        PrintStat( "elapsed time:", totalTime / CTimed::NanoPerMilli() );

        FILETIME creationFT, exitFT, kernelFT, userFT;
        if ( GetProcessTimes( GetCurrentProcess(), &creationFT, &exitFT, &kernelFT, &userFT ) )
        {
            ULARGE_INTEGER ullK, ullU;
            ullK.HighPart = kernelFT.dwHighDateTime;
            ullK.LowPart = kernelFT.dwLowDateTime;
    
            ullU.HighPart = userFT.dwHighDateTime;
            ullU.LowPart = userFT.dwLowDateTime;
    
            PrintStat( "kernel CPU:", ullK.QuadPart / 10000 );
            PrintStat( "user CPU:", ullU.QuadPart / 10000 );
            PrintStat( "total CPU:", ( ullU.QuadPart + ullK.QuadPart ) / 10000 );
        }
    }

    tracer.Shutdown();
    return 0;
} //wmain

