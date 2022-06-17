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
#include <gdiplus.h>

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
using namespace Gdiplus;

#include <djltrace.hxx>
#include <djlenum.hxx>
#include <djl_pa.hxx>
#include <djltimed.hxx>
#include <djl_wav.hxx>
#include <djl_kmeans.hxx>
#include <djl_kdtree.hxx>
#include <warp_sort.hxx>

#pragma comment( lib, "ole32.lib" )
#pragma comment( lib, "shlwapi.lib" )
#pragma comment( lib, "oleaut32.lib" )
#pragma comment( lib, "windowscodecs.lib" )
#pragma comment( lib, "gdi32.lib" )
#pragma comment( lib, "user32.lib" )
#pragma comment( lib, "Gdiplus.lib" )

CDJLTrace tracer;
std::mutex g_mtxGDI;
ComPtr<IWICImagingFactory> g_IWICFactory;
long long g_CollagePrepTime = 0;
long long g_CollageStitchTime = 0;
long long g_CollageStitchFloodTime = 0;
long long g_CollageStitchReadPixelsTime = 0;
long long g_CollageStitchDrawTime = 0;
long long g_CollageWriteTime = 0;
long long g_ColorizeImageTime = 0;
long long g_ShowColorsAllTime = 0;
long long g_ShowColorsOpenTime = 0;
long long g_ShowReadPixelsTime = 0;
long long g_ShowColorsFeaturizeClusterTime = 0;
long long g_ShowColorsClusterRunTime = 0;
long long g_ShowColorsPostClusterTime = 0;
long long g_ShowColorsPaletteTime = 0;
long long g_PosterizePixelsTime = 0;
long long g_ReadPixelsTime = 0;
long long g_WritePixelsTime = 0;
long long g_ShowColorsCopyTime = 0;
long long g_ShowColorsSortTime = 0;
long long g_ShowColorsUniqueTime = 0;
long long g_ShowColorsClusterFeatureSelectionTime = 0;

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

enum ColorMapping { mapNone, mapColor, mapBrightness, mapHue, mapSaturation, mapGradient };

union ColorBytes
{
    ColorBytes( DWORD d ) : dw( d ) {}

    struct { byte l, m, h, zero; };
    struct { byte b, g, r, bgrzero; };
    struct { byte h, s, v, hsvzero; };
    DWORD dw;
};

struct ColorizationData
{
    ColorizationData() : mapping( mapNone ) {}
    ColorMapping mapping;
    vector<DWORD> bgrdata;
    vector<byte> hsvdata;        // may contain h, s, or v depending on mapping
    unique_ptr<KDTreeBGR> kdtree;
};

const int sixtyDegrees = 42; // 60 out of 360, and 42 out of 256 (42 * 6 = 252)

int RGBToV( int r, int g, int b )
{
    int v = ( __max( __max( r, g ), b ) );
    assert( v >= 0 );
    assert( v <= 255 );
    return v;
} //RGBToV

void RGBToHSV( int r, int g, int b, int & h, int & s, int & v )
{
    int min; // note: v == max.
    int diff;

    if ( r > g )
    {
        if ( g < b )
            min = g;
        else
            min = b;

        if ( r > b )
        {
            v = r;
            diff = v - min;
            h = ( sixtyDegrees * ( g - b ) ) / diff;
            if ( h < 0 )
                h += ( 6 * sixtyDegrees );
        }
        else
        {
            v = b;
            diff = v - min;
            h = ( 4 * sixtyDegrees ) + ( ( sixtyDegrees * ( r - g ) ) / diff );
        }
    }
    else if ( g > b )
    {
        v = g;

        if ( r < b )
            min = r;
        else
            min = b;

        diff = v - min;
        h = ( 2 * sixtyDegrees ) + ( ( sixtyDegrees * ( b - r ) ) / diff );

    }
    else
    {
        v = b;
        min = r;
        diff = b - r;
        if ( 0 != diff )
            h = ( 4 * sixtyDegrees ) + ( ( sixtyDegrees * ( r - g ) ) / diff );
        else
            h = 0;
    }

    if ( 0 == v )
    {
        h = 0;
        s = 0;
        return;
    }

    s = ( 255 * diff ) / v;

    if ( 0 == s )
        h = 0;

    assert( h >= 0 );
    assert( s >= 0 );
    assert( v >= 0 );
    assert( h <= 255 );
    assert( s <= 255 );
    assert( v <= 255 );
} //RGBToHSV

void BGRToHSV( DWORD color, int & h, int & s, int & v )
{
    int b = color & 0xff;
    int g = ( color >> 8 ) & 0xff;
    int r = ( color >> 16 ) & 0xff;
    RGBToHSV( r, g, b, h, s, v );
} //BGRToHSV

template <class T> void Swap( T & a, T & b )
{
    T c = a;
    a = b;
    b = c;
} //Swap

int compare_brightness( const void * a, const void * b )
{
    ColorBytes cba( * (DWORD *) a );
    ColorBytes cbb( * (DWORD *) b );

    // the value is defined as the brightest channel. No additional work is done for ties

    int maxvala = RGBToV( cba.r, cba.g, cba.b );
    int maxvalb = RGBToV( cbb.r, cbb.g, cbb.b );

    return maxvala - maxvalb;
} //compare_brightness

int compare_hue( const void * a, const void * b )
{
    DWORD ca = * (DWORD *) a;
    DWORD cb = * (DWORD *) b;

    int ha, sa, va;
    BGRToHSV( ca, ha, sa, va );
    int hb, sb, vb;
    BGRToHSV( cb, hb, sb, vb );

    return ha - hb;
} //compare_hue

int compare_saturation( const void * a, const void * b )
{
    DWORD ca = * (DWORD *) a;
    DWORD cb = * (DWORD *) b;

    int ha, sa, va;
    BGRToHSV( ca, ha, sa, va );
    int hb, sb, vb;
    BGRToHSV( cb, hb, sb, vb );

    return sa - sb;
} //compare_saturation

int compare_byte( const void * a, const void * b )
{
    int ia = (int) * (byte *) a;
    int ib = (int) * (byte *) b;

    return ia - ib;
} //compare_byte

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
        //printf( "bitmap source is 48bppRGB and that's OK!\n" );
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

HRESULT ScaleWICBitmap( ComPtr<IWICBitmapSource> & source, int longEdge, bool highQualityScaling )
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

    hr = scaler->Initialize( source.Get(), targetWidth, targetHeight,
                             highQualityScaling ? WICBitmapInterpolationModeHighQualityCubic :
                                                  WICBitmapInterpolationModeNearestNeighbor );
    if ( FAILED( hr ) )
    {
        printf( "can't initialize scaler: %#x\n", hr );
        return hr;
    }

    source.Reset();
    source.Attach( scaler.Detach() );

    return hr;
} //ScaleWICBitmap

HRESULT ClipWICBitmap( ComPtr<IWICBitmapSource> & source, int outputWidth, int outputHeight )
{
    UINT width, height;

    HRESULT hr = source->GetSize( &width, &height );
    if ( FAILED( hr ) )
    {
        printf( "can't get size of input bitmap: %#x\n", hr );
        return hr;
    }

    double targetAspectRatio = (double) outputWidth / (double) outputHeight;
    double inputAspectRatio = (double) width / (double) height;

    UINT targetWidth, targetHeight;
    WICRect roiRect;

    if ( targetAspectRatio >= inputAspectRatio )
    {
        roiRect.X = 0;
        roiRect.Width = width;
        roiRect.Height = (int) ( (double) width / targetAspectRatio );
        roiRect.Y = ( height - roiRect.Height ) / 2;
    }
    else
    {
        roiRect.Y = 0;
        roiRect.Height = height;
        roiRect.Width = (int) ( (double) height * targetAspectRatio );
        roiRect.X = ( width - roiRect.Width ) / 2;
    }

    //printf( "original dimensions %u by %u, clip.x %d, clip.width %d, clip.y %d, clip.height %d\n",
    //        width, height, roiRect.X, roiRect.Width, roiRect.Y, roiRect.Height );

    ComPtr<IWICBitmapClipper> clipper;
    hr = g_IWICFactory->CreateBitmapClipper( clipper.GetAddressOf() );
    if ( FAILED( hr ) )
    {
        printf( "can't create a clipper: %#x\n", hr );
        return hr;
    }

    hr = clipper->Initialize( source.Get(), &roiRect );
    if ( FAILED( hr ) )
    {
        printf( "can't initialize clipper: %#x\n", hr );
        return hr;
    }

    source.Reset();
    source.Attach( clipper.Detach() );

    return hr;
} //ClipWICBitmap

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

template <class T> void FloodFill( T * buffer, int width, int height, int fillColor )
{
    // Only written and tested for 24bppBGR and 48bppRGB

    int bitsShiftLeft = 8 * ( sizeof T - 1 );
    T fillRed = ( ( fillColor & 0xff0000 ) >> 16 ) << bitsShiftLeft;
    T fillGreen = ( ( fillColor & 0xff00 ) >> 8 ) << bitsShiftLeft;
    T fillBlue = ( fillColor & 0xff ) << bitsShiftLeft;

    if ( 2 == sizeof( T ) )
        Swap( fillRed, fillBlue );

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

                // The input/output image is either 24bppGBR or 48bppRGB

                T grey = ( 1 == sizeof( T ) ) ? MakeGreyscale( r, g, b ) : MakeGreyscale( b, g, r );

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
    WCHAR awcWAV[ MAX_PATH ];
    wcscpy( awcWAV, pwcWAVBase );
    wcscat( awcWAV, L".wav" );
    vector<short> wav;
    int samples;
    
    if ( 1 == waveMethod || 2 == waveMethod )
    {
        // stretch the image as much as needed to make a wav file that contains all on pixels

        bool allPixels = ( 1 == waveMethod) ? false : true;  // true == airplane?  false == fart?
        samples = allPixels ? width * height : CountPixelsOn( image, width, height, stride );
        wav.resize( samples );
        ZeroMemory( wav.data(), samples * sizeof( short ) );
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
    
                    wav[ cur++ ] = v;
                }
                else if ( allPixels )
                    wav[ cur++ ] = -32768;
            }
        }
    }
    else if ( 3 == waveMethod || 4 == waveMethod )
    {
        byte *pb = image;
        vector<short> maxY( width );
        ZeroMemory( maxY.data(), width * sizeof( short ) );

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
                                maxY[ x ] = (short) ( (double) ( height - y ) / (double) height * 32767 );
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
                                maxY[ x ] = (short) ( (double) ( height - y ) / (double) height * 32767 );
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
                    maxY[ x ] = height / 2;
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
                        maxY[ x ] = (short) ( (double) ( height - y ) / (double) height * 32767 );
                    }
                }
    
                // if none set, use the midpoint
    
                if ( 0 == brightestY )
                {
                    byte * p = pb + ( ( height / 2 ) * stride ) + ( x * 3 );
                    *p++ = 255; *p++ = 255; *p = 255;
                    maxY[ x ] = height / 2;
                }
            }
        }

        // The image is now the first half of the waveform. Invert it to make the second half.
        // Target A above middle C -- 440 Hz at 88200 samples per second, for one second of sound

        samples = 88200;
        int samplesPerWave = (int) round( (double) 1.0 / (double) 440.0 * (double) samples );
        int halfSamplesPerWave = samplesPerWave / 2;
        printf( "width %d, samplesPerWave %d, halfSamplesPerWave %d\n", width, samplesPerWave, halfSamplesPerWave );

        wav.resize( samples );
        ZeroMemory( wav.data(), samples * sizeof( short ) );

        for ( int t = 0; t < halfSamplesPerWave; t++ )
        {
            int x = (int) round( (double) t / (double) halfSamplesPerWave * (double) width );
            wav[ t ] = maxY[ x ];
            wav[ t + halfSamplesPerWave ] = - maxY[ x ];
        }

        // replicate the wave throughout the buffer

        int copies = samples / samplesPerWave;
        for ( int c = 1; c < copies; c++ )
            memcpy( wav.data() + c * samplesPerWave, wav.data(), samplesPerWave * sizeof( short ) );
    }

    // write the WAV to disk

    DjlParseWav::WavSubchunk fmtOut( 1, 1, sampleRate, bytesPerSample, 8 * bytesPerSample );
    DjlParseWav output( awcWAV, fmtOut );
    if ( !output.OpenSuccessful() )
    {
        printf( "can't open WAV output file %ws\n", awcWAV );
        return;
    }
    
    bool ok = output.WriteWavFile( (byte *) wav.data(), samples * sizeof( short ) );
    if ( !ok )
        printf( "can't write WAV file %ws\n", awcWAV );
    else
        printf( "created WAV file: %ws\n", awcWAV );
} //CreateWAVFromImage

template <class T> __forceinline T Posterize( T * pt, int groupSpan, vector<int> & values )
{
    // posterization level 1 values: 255         // it'll be all white
    //                     2       : 0 and 255
    //                     3       : 0, 127, 255
    //                     4       : 0, 85, 170, 255
    //                     5       : 0, 63, 127, 191, 255
    //                     6       : 0, 51, 102, 153, 204, 255
    //                     7       : 0, 42, 85, 127, 170, 212, 255

    int element = *pt / groupSpan;
    assert( element < values.size() );
    return values[ element ];
} //Posterize

template <class T> void PosterizeImage( T * image, int stride, int width, int height, int posterizeLevel )
{
    CTimed timePosterize( g_PosterizePixelsTime );
    assert( 0 != posterizeLevel );
    const int maxT = ( 1 << sizeof( T ) * 8 ) - 1;
    const int groupSpan = ( maxT + 1 ) / posterizeLevel;

    // use the vector so double math can compute the values once

    vector<int> values( posterizeLevel + 1 );
    for ( int v = 0; v < posterizeLevel; v++ )
        values[ v ] = floor( (double) v * (double) maxT / (double) ( posterizeLevel - 1 ) );

    values[ posterizeLevel - 1 ] = maxT; // make the brightest truly bright
    values[ posterizeLevel ] = maxT;     // for cases like 5 when 255 is divisible by 51

    for ( int y = 0; y < height; y++ )
    {
        T * pRow = (T *) ( (byte *) image + y * stride );
    
        for ( int x = 0; x < width; x++ )
        {
            *pRow++ = Posterize( pRow, groupSpan, values );
            *pRow++ = Posterize( pRow, groupSpan, values );
            *pRow++ = Posterize( pRow, groupSpan, values );
        }
    }
} //PosterizeImage

long ColorDistance( int ra, int ga, int ba, int rb, int gb, int bb )
{
    long diffr = ra - rb;
    long diffg = ga - gb;
    long diffb = ba - bb;
    return ( diffr * diffr ) + ( diffg * diffg ) + ( diffb * diffb );
} //ColorDistance

long ColorDistance( int r, int g, int b, DWORD color2 )
{
    return ColorDistance( r, g, b, ( color2 >> 16 ) & 0xff, ( color2 >> 8 ) & 0xff, color2 & 0xff );
} //ColorDistance

DWORD FindNearestColor( int r, int g, int b, ColorizationData & cd, int posterizeLevel )
{
    assert( posterizeLevel <= cd.bgrdata.size() );

    if ( mapColor == cd.mapping )
    {
        DWORD idnearest;
        cd.kdtree->Nearest( r, g, b, idnearest );

        assert( idnearest < posterizeLevel );
        return idnearest;
    }

    if ( mapGradient == cd.mapping )
    {
        int v = RGBToV( r, g, b );
        double dv = (double) v / 255.0;
        int bucket = (int) ( dv * (double) posterizeLevel );
        if ( bucket == posterizeLevel ) // v of 255 will hit this; round down
            bucket--;

        assert( bucket >= 0 );
        assert( bucket < posterizeLevel );

        return bucket;
    }

    assert( mapHue == cd.mapping || mapSaturation == cd.mapping || mapBrightness == cd.mapping );

    int h, s, v;
    RGBToHSV( r, g, b, h, s, v );

    byte val = ( mapHue == cd.mapping ) ? h : ( mapSaturation == cd.mapping ) ? s : v;

    // lower_bound finds the first item >= to the search item. The closest match may be that
    // or the prior item. lower_bound should be n log(n), rather than linear

    auto iterGE = std::lower_bound( cd.hsvdata.begin(), cd.hsvdata.end(), val );
    int nearest = 0;
    if ( iterGE == cd.hsvdata.end() )
        nearest = cd.hsvdata.size() - 1;
    else
    {
        DWORD index = std::distance( cd.hsvdata.begin(), iterGE );
        if ( index > 0 )
        {
            byte ival = cd.hsvdata[ index ];
            byte im1val = cd.hsvdata[ index - 1 ];

            if ( abs( (int) ival - (int) val ) < abs( (int) im1val - (int) val ) )
                nearest = index;
            else
                nearest = index - 1;
        }
    }

    return nearest;
} //FindNearestColor

// Posterize, but use the specified colors to map to brightness

template <class T> void ColorizeImage( T * image, int stride, int width, int height, int posterizeLevel, ColorizationData * colorizationData )
{
    CTimed timeColorize( g_ColorizeImageTime );

    assert( 0 != posterizeLevel );
    const int maxT = ( 1 << sizeof( T ) * 8 ) - 1;
    const int groupSpan = ( maxT + 1 ) / posterizeLevel;
    const DWORD count = colorizationData->bgrdata.size();
    //printf( "colorizing posterize %d, colors %zd, method %d\n", posterizeLevel, colorizationData->bgrdata.size(), colorizationData->mapping );

    //for ( int y = 0; y < height; y++ )
    parallel_for ( 0, height, [&] ( int y )
    {
        T * pRow = (T *) ( (byte *) image + y * stride );
    
        for ( int x = 0; x < width; x++ )
        {
            byte r, g, b;

            // The input/output image is either 24bppGBR or 48bppRGB

            if ( 1 == sizeof( T ) )
            {
                b = pRow[ 0 ];
                g = pRow[ 1 ];
                r = pRow[ 2 ];
            }
            else
            {
                assert( 2 == sizeof( T ) );

                r = (unsigned short) pRow[ 0 ] >> 8;
                g = (unsigned short) pRow[ 1 ] >> 8;
                b = (unsigned short) pRow[ 2 ] >> 8;
            }

            DWORD index = FindNearestColor( r, g, b, *colorizationData, posterizeLevel );

            // index = __min( count - 1, y / ( height / posterizeLevel ) );      // testing to see all colors

            assert( index < count );
            DWORD c = colorizationData->bgrdata[ index ];
            ColorBytes cb( c );

            if ( 1 == sizeof( T ) )
            {
                pRow[ 0 ] = cb.b;
                pRow[ 1 ] = cb.g;
                pRow[ 2 ] = cb.r;
            }
            else
            {
                pRow[ 0 ] = ( (T) cb.r ) << 8;
                pRow[ 1 ] = ( (T) cb.g ) << 8;
                pRow[ 2 ] = ( (T) cb.b ) << 8;
            }

            pRow += 3;
        }
    } );
} //ColorizeImage

int compare_colors( const void * a, const void * b )
{
    int ca = * (int *) a;
    int cb = * (int *) b;

    return ca - cb;
} //compare_colors

unsigned long lrand()
{
    unsigned long r = 0;

    for ( int i = 0; i < 5; i++ )
        r = ( r << 15 ) | ( rand() & 0x7FFF );

    return r;
} //lrand

HRESULT PngFromVector( vector<DWORD> & vec, WCHAR const * pwcFile )
{
    int width = 128;
    int height = vec.size() / width;
    if ( vec.size() % width )
        height++;

    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> bitmapFrameEncode;
    HRESULT hr = CreateWICEncoder( pwcFile, encoder, bitmapFrameEncode, L"image/png", false );
    if ( FAILED( hr ) )
    {
        printf( "can't create a wic encoder, error %#x\n", hr );
        return hr;
    }
    
    hr = bitmapFrameEncode->SetSize( width, height );
    if ( FAILED( hr ) )
    {
        printf( "failed to set encoder frame size %d, %d, error %#x\n", width, height, hr );
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
    
    int bppOut = g_BitsPerPixel;
    int strideOut = StrideInBytes( width, bppOut );
    int cbOut = strideOut * height;
    vector<byte> bufferOut( cbOut );
    ZeroMemory( bufferOut.data(), cbOut );
    printf( "buffer size: %d\n", cbOut );
    printf( "vec size %d\n", (int) vec.size() );
    printf( "width %d, height %d\n", width, height );

    for ( int y = 0; y < height; y++ )
    {
        for ( int x = 0; x < width; x++ )
        {
            byte * p = bufferOut.data() + y * strideOut + 3 * x;
            int vecoffset = y * width + x;
            if ( vecoffset < vec.size() )
            {
                ColorBytes cb( vec[ y * width + x ] );
                *p++ = cb.b;
                *p++ = cb.g;
                *p++ = cb.r;
            }
        }
    }
    
    hr = bitmapFrameEncode->WritePixels( height, strideOut, cbOut, bufferOut.data() );
    if ( FAILED( hr ) )
    {
        printf( "failed to write pixels %#x\n", hr );
        return hr;
    }
    
    hr = CommitEncoder( bitmapFrameEncode, encoder );
    if ( FAILED( hr ) )
    {
        printf( "failed to commit the encoder %#x\n", hr );
        return hr;
    }

    return hr;
} //PngFromVector

template <class T> void ShowColorsFromBuffer( T * p, int bpp, int stride, int width, int height,
                                              int showColorCount, vector<DWORD> & centroids,
                                              bool printColors )
{
    // store all the colors in a DWORD vector, removing adjacent duplicates

    vector<DWORD> colors;

    {
        CTimed showColorsCopyTime( g_ShowColorsCopyTime );
        T * row = p;
        DWORD prevColor = 0xffffffff;
        vector<DWORD> rowColors;
    
        for ( int y = 0; y < height; y++ )
        {
            rowColors.clear();
            T * pixel = row;
        
            for ( int x = 0; x < width; x++ )
            {
                byte r, g, b;
    
                // The input/output image is either 24bppGBR or 48bppRGB
    
                if ( 1 == sizeof( T ) )
                {
                    b = pixel[ 0 ];
                    g = pixel[ 1 ];
                    r = pixel[ 2 ];
                }
                else
                {
                    assert( 2 == sizeof( T ) );
    
                    r = (byte) ( (unsigned short) pixel[ 0 ] >> 8 );
                    g = (byte) ( (unsigned short) pixel[ 1 ] >> 8 );
                    b = (byte) ( (unsigned short) pixel[ 2 ] >> 8 );
                }
    
                DWORD color = b | ( g << 8 ) | ( r << 16 );
    
                if ( color != prevColor )
                {
                    colors.push_back( color );
                    prevColor = color;
                }
    
                pixel += 3;
            }
    
            row += ( stride * sizeof( T ) );
        }
    }

    {
        CTimed showColorsSortTime( g_ShowColorsSortTime );

        //qsort( colors.data(), colors.size(), sizeof( DWORD ), compare_colors );

         // 30% faster than qsort
        //std::sort( colors.begin(), colors.end() );

        // this sort is 20% faster than std::sort for interesting use cases
        MedianHybridQuickSort( colors.data(), colors.size() );
    }

    // copy unique colors

    vector<DWORD> uniqueColors;

    {
        CTimed showColorsUniqueTime( g_ShowColorsUniqueTime );
        DWORD prevColor = 0xffffffff;
    
        for ( int i = 0; i < colors.size(); i++ )
        {
            if ( colors[ i ] != prevColor )
            {
                uniqueColors.push_back( colors[ i ] );
                prevColor = colors[ i ];
            }
        }
    }

    showColorCount = __min( showColorCount, uniqueColors.size() );

    if ( printColors )
    {
        printf( "pixels in image:   %12d\n", height * width );
        printf( "first-pass unique: %12zd\n", colors.size() );
        printf( "unique colors:     %12zd\n", uniqueColors.size() );
        printf( "shown colors:      %12d\n", showColorCount );

        //for ( int i = 0; i < uniqueColors.size(); i++ )
        //    printf( "  color %d: %#x\n", i, uniqueColors[ i ] );
    }

    CTimed showColorsClusterFeatureSelectionTime( g_ShowColorsClusterFeatureSelectionTime );

    colors.clear();

    // get a sample set of the colors for clustering

    const int sampleSize = 10000;

    if ( uniqueColors.size() <= showColorCount )
    {
        // no need to cluster -- just use the unique colors

        for ( int i = 0; i < uniqueColors.size(); i++ )
            centroids.push_back( uniqueColors[ i ] );

        showColorsClusterFeatureSelectionTime.Complete();
    }
    else
    {
        int clusteredColorCount = __max( showColorCount, __min( sampleSize, uniqueColors.size() ) );
        if ( printColors )
            printf( "clusteredColorCount: %10d\n", clusteredColorCount );
    
        vector<DWORD> clusteredColors( clusteredColorCount );
    
        srand( time( 0 ) );
        for ( int i = 0; i < clusteredColorCount; i++ )
            clusteredColors[ i ] = uniqueColors[ lrand() % uniqueColors.size() ];

        uniqueColors.clear();
        showColorsClusterFeatureSelectionTime.Complete();

        // cluster the sample set
    
        vector<KMeansPoint> all_points;
        {
            CTimed showColorsFeaturizeClusterTime( g_ShowColorsFeaturizeClusterTime );
        
            for ( int i = 0; i < clusteredColorCount; i++ )
            {
                ColorBytes cb( clusteredColors[ i ] );
                all_points.emplace_back( i, cb.r, cb.g, cb.b );
            }
        }
    
        const int iters = 100;
        const int K = showColorCount;
        KMeans kmeans( K, iters );

        {
            CTimed showColorsClusterRunTime( g_ShowColorsClusterRunTime );
            kmeans.run( all_points );
        }

        {
            CTimed showColorsClusterRunTime( g_ShowColorsPostClusterTime );
    
            kmeans.sort();
            //kmeans.getbgrSynthetic( centroids ); // get the synthetic color centroids; they may not be in actual image
            kmeans.getbgrClosest( centroids );  // get the actual image colors closest to the centoids
    
            #ifndef NDEBUG // ensure clustering gave back colors from the original set
                for ( int i = 0; i < centroids.size(); i++ )
                {
                    bool found = false;
                    for ( int j = 0; j < clusteredColors.size(); j++ )
                    {
                        if ( centroids[ i ] == clusteredColors[ j ] )
                        {
                            found = true;
                            break;
                        }
                    }
                    assert( found );
                }
            #endif
        }
    }

    if ( printColors )
    {
        printf( "centroid colors ordered by cluster size:\n" );
        printf( "static DWORD ColorizationColors%zd[] =\n{", centroids.size() );

        for ( int i = 0; i < centroids.size(); i++ )
        {
            if ( 0 == ( i % 8 ) )
                printf( "\n    " );
            printf( "%#08x, ", centroids[ i ] );
        }
        printf( "\n};\n" );
    }
} //ShowColorsFromBuffer

HRESULT ShowColors( WCHAR const * input, int showColorCount, vector<DWORD> & centroids, bool printColors,
                    WCHAR const * pwcOutput = 0, WCHAR const * outputMimetype = 0 )
{
    // GDI is single-threaded

    lock_guard<mutex> lock( g_mtxGDI );

    CTimed timedShowColorsAll( g_ShowColorsAllTime );
    CTimed timedShowColorsOpen( g_ShowColorsOpenTime );

    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICBitmapFrameDecode> frame;

    HRESULT hr = LoadWICBitmap( input, source, frame, true );
    if ( FAILED( hr ) )
    {
        printf( "can't load wic bitmap %#x\n", hr );
        return hr;
    }

    UINT width = 0;
    UINT height = 0;

    hr = source->GetSize( &width, &height );
    if ( FAILED( hr ) )
    {
        printf( "can't get dimensions of path %ws\n", input );
        return hr;
    }

    timedShowColorsOpen.Complete();

#if false // even though this can be much faster, WIC modifies colors when resizing, even if there are 2 colors!
    const UINT maxSourceImageDimension = 1000;

    if ( width > maxSourceImageDimension || height > maxSourceImageDimension )
    {
        hr = ScaleWICBitmap( source, maxSourceImageDimension );
        if ( FAILED( hr ) )
        {
            printf( "can't scale the input bitmap, error %#x\n", hr );
            return hr;
        }

        hr = source->GetSize( &width, &height );
        if ( FAILED( hr ) )
        {
            printf( "can't get dimensions after scaling of path %ws\n", input );
            return hr;
        }
    }
#endif

    CTimed showReadPixels( g_ShowReadPixelsTime );
    int bppIn = g_BitsPerPixel;
    int strideIn = StrideInBytes( width, bppIn );

    int cbIn = strideIn * height;
    vector<byte> bufferIn( cbIn );

    hr = source->CopyPixels( 0, strideIn, cbIn, bufferIn.data() );
    showReadPixels.Complete();
    if ( FAILED( hr ) )
    {
        printf( "ShowColors() failed to read input pixels in CopyPixels() %#x\n", hr );
        return hr;
    }

    ShowColorsFromBuffer( bufferIn.data(), bppIn, strideIn, width, height, showColorCount, centroids, printColors );

    if ( pwcOutput )
    {
        CTimed showColorsPalette( g_ShowColorsPaletteTime );

        // create an output bitmap 128 pixels wide with a 16 pixel band for each color
        const UINT width = 128;
        const UINT bandHeight = 16;

        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> bitmapFrameEncode;
        HRESULT hr = CreateWICEncoder( pwcOutput, encoder, bitmapFrameEncode, outputMimetype, false );
        if ( FAILED( hr ) )
        {
            printf( "can't create a wic encoder, error %#x\n", hr );
            return hr;
        }
    
        UINT height = bandHeight * centroids.size();
        hr = bitmapFrameEncode->SetSize( width, height );
        if ( FAILED( hr ) )
        {
            printf( "failed to set encoder frame size %d, %d, error %#x\n", width, height, hr );
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
    
        int bppOut = g_BitsPerPixel;
        int strideOut = StrideInBytes( width, bppOut );
        int cbOut = strideOut * height;

        // write the RGB values over color bars for each entry in the centroids palette

        HFONT hfont = CreateFont( bandHeight - 1, 0, 0, 0, FW_THIN, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_OUTLINE_PRECIS,
                                  CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas" );
        if ( 0 == hfont )
        {
            hr = HRESULT_FROM_WIN32( GetLastError() );
            printf( "can't create a font, error %#x\n", hr );
            return hr;
        }

        HDC memdc = CreateCompatibleDC( GetDC( 0 ) );
        if ( 0 == memdc )
        {
            DeleteObject( hfont );
            hr = HRESULT_FROM_WIN32( GetLastError() );
            printf( "can't create a compatible dc, error %#x\n", hr );
            return hr;
        }

        BITMAPINFO bmi;
        ZeroMemory( &bmi, sizeof bmi );
        bmi.bmiHeader.biSize = sizeof bmi;
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height; // negative means top-down dib
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;   
        byte *pbBits;
        HBITMAP bitmap = CreateDIBSection( GetDC( 0 ), &bmi, DIB_RGB_COLORS, (void **) &pbBits, 0, 0 );
        if ( 0 == bitmap )
        {
            DeleteObject( hfont );
            DeleteDC( memdc );
            hr = HRESULT_FROM_WIN32( GetLastError() );
            printf( "can't create a DIBSection, error %#x\n", hr );
            return hr;
        }

        HBITMAP oldbitmap = (HBITMAP) SelectObject( memdc, bitmap );
        HFONT fontOld = (HFONT) SelectObject( memdc, hfont );

        for ( int c = 0; c < centroids.size(); c++ )
        {
            int yoffset = c * bandHeight;
            RECT rc = { 0, (LONG) yoffset, (LONG) width, (LONG) ( yoffset + bandHeight ) };
            ColorBytes cb( centroids[ c ] );
            Swap( cb.r, cb.b ); // GDI APIs take RGB, but the data in pvBits appears to be BGR
            DWORD colorRGB = cb.dw;
            COLORREF crBkOld = SetBkColor( memdc, colorRGB );
            COLORREF crText = ColorDistance( 0, 0, 0, colorRGB ) < ColorDistance( 255, 255, 255, colorRGB ) ? 0xffffff : 0;
            COLORREF crTextOld = SetTextColor( memdc, crText );
            HBRUSH brush = CreateSolidBrush( colorRGB );
            FillRect( memdc, &rc, brush );
            DeleteObject( brush );

            // RGB hex standard notation is actually BGR (blue is lowest byte on the right).
            // the width in %#06x for wsprintf is different than printf. printf includes the 0x in the
            // width and wsprintf does not.

            static WCHAR awcColor[ 30 ] = {0};
            wsprintf( awcColor, L"%#06x", centroids[ c ] );
            TextOut( memdc, 1, 1 + yoffset, awcColor, wcslen( awcColor ) );

            SetTextColor( memdc, crTextOld );
            SetBkColor( memdc, crBkOld );
        }

        GdiFlush(); // make sure all the writes above are complete
        hr = bitmapFrameEncode->WritePixels( height, strideOut, cbOut, pbBits );

        // delay hr check until after resources are freed

        SelectObject( memdc, fontOld );
        SelectObject( memdc, oldbitmap );
        DeleteObject( hfont );
        DeleteDC( memdc );
        DeleteObject( bitmap );
    
        if ( FAILED( hr ) )
        {
            printf( "failed to write pixels %#x\n", hr );
            return hr;
        }
    
        hr = CommitEncoder( bitmapFrameEncode, encoder );
        if ( FAILED( hr ) )
        {
            printf( "failed to commit the encoder %#x\n", hr );
            return hr;
        }
    }

    return hr;
} //ShowColors

void DrawCaption( const WCHAR * pwcPath, byte * pOut, int stride, int xOffset, int yOffset,
                  int width, int height, int fullWidth, int fullHeight, int bpp,
                  bool fontSizeRelativeToWidth = true )
{
    // GDI is single-threaded

    lock_guard<mutex> lock( g_mtxGDI );

    // rgb vs bgr doesn't matter because text is black and white

    PixelFormat pixelFormat = ( 24 == bpp ) ? PixelFormat24bppRGB : PixelFormat32bppRGB;

    // Get just the filename from the path to use as a caption

    WCHAR caption[ MAX_PATH ];
    const WCHAR * slash = wcsrchr( pwcPath, L'\\' );
    if ( slash )
        wcscpy( caption, slash + 1 );
    else
        wcscpy( caption, pwcPath );

    WCHAR * dot = wcsrchr( caption, L'.' );
    if ( dot )
        *dot = 0;

    // Create a gdi+ bitmap and write the text. Use paths for multiple colors
    // so that the text appears on any image instead of blending into similar colors.

    Bitmap bitmap( fullWidth, fullHeight, stride, pixelFormat, pOut );

    FontFamily fontFamily( L"Arial" );
    Font font( &fontFamily, 12, FontStyleBold, UnitPoint );

    StringFormat stringFormat;
    stringFormat.SetAlignment( StringAlignmentCenter );
    stringFormat.SetLineAlignment( StringAlignmentCenter );

    RectF rect( (REAL) xOffset, (REAL) yOffset + (REAL) height * 3.0 / 4.0,
                (REAL) width, (REAL) height / 4.0 );

    Graphics graphics( & bitmap );
    graphics.SetSmoothingMode( SmoothingModeAntiAlias );
    graphics.SetInterpolationMode( InterpolationModeHighQualityBicubic );

    GraphicsPath path;
    int fontSize = ( fontSizeRelativeToWidth ? width : height ) / 16;
    path.AddString( caption, wcslen( caption ), &fontFamily, FontStyleRegular, fontSize, rect, &stringFormat );
    Pen pen( Color( 255, 255, 255 ), 4 );
    pen.SetLineJoin( LineJoinRound );
    graphics.DrawPath( &pen, &path);
    SolidBrush brush( Color( 0, 0, 0 ) );
    graphics.FillPath( &brush, &path );
} //DrawCaption

// Note: this is effectively a blt -- there is no stretching or scaling.

HRESULT DrawImage( byte * pOut, int strideOut, ComPtr<IWICBitmapSource> & source, int waveMethod, const WCHAR * pwcWAVBase,
                   int posterizeLevel, ColorizationData * colorizationData, bool makeGreyscale, int offsetX, int offsetY,
                   int width, int height, int bppIn, int bppOut )
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
    vector<byte> bufferIn( cbIn );
    
    {
        CTimed timed( g_CollageStitchReadPixelsTime );
        CTimed timedReadPixels( g_ReadPixelsTime );

        // Almost all of the runtime of this app will be in source->CopyPixels(), where the input file is parsed and scaled

        HRESULT hr = source->CopyPixels( 0, strideIn, cbIn, bufferIn.data() );
        if ( FAILED( hr ) )
        {
            printf( "DrawImage() failed to read input pixels in CopyPixels() %#x\n", hr );
            return hr;
        }
    }

    CTimed stitchDraw( g_CollageStitchDrawTime );
    int bytesppOut = bppOut / 8;
    byte * pbOutBase = pOut + ( offsetY * strideOut ) + ( offsetX * bytesppOut );
    byte * pbInBase = bufferIn.data();

    if ( 24 == bppIn )
        CopyPixels( 0, height, width, pbOutBase, pbInBase, strideOut, strideIn, makeGreyscale );
    else
        CopyPixels( 0, height, width, (USHORT *) pbOutBase, (USHORT *) pbInBase, strideOut, strideIn, makeGreyscale );

    if ( 0 != colorizationData )
    {
        assert( 0 != posterizeLevel );

        if ( 24 == bppOut )
            ColorizeImage( pbOutBase, strideOut, width, height, posterizeLevel, colorizationData );
        else
            ColorizeImage( (USHORT *) pbOutBase, strideOut, width, height, posterizeLevel, colorizationData );
    }
    else  if ( 0 != posterizeLevel )
    {
        if ( 24 == bppOut )
            PosterizeImage( pbOutBase, strideOut, width, height, posterizeLevel );
        else
            PosterizeImage( (USHORT *) pbOutBase, strideOut, width, height, posterizeLevel );
    }

    if ( 0 != waveMethod )
       CreateWAVFromImage( waveMethod, pwcWAVBase, pbOutBase, strideOut, width, height, bppOut );

    return S_OK;
} //DrawImage

HRESULT WriteWICBitmap( WCHAR const * pwcOutput, ComPtr<IWICBitmapSource> & source, ComPtr<IWICBitmapFrameDecode> & frame,
                        int longEdge, int waveMethod, int posterizeLevel, ColorizationData * colorizationData,
                        bool makeGreyscale, double aspectRatio, int fillColor, WCHAR const * outputMimetype,
                        bool lowQualityOutput, bool gameBoy, bool highQualityScaling )
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

    if ( !gameBoy && ( ( ( 0.0 == aspectRatio ) || SameDouble( aspectRatio, originalAspectRatio ) ) && ( 0 == posterizeLevel ) && ( 0 == waveMethod ) ) )
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
            hr = ScaleWICBitmap( source, longEdge, highQualityScaling );
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

        if ( gameBoy )
        {
            wOut = 128;
            hOut = 112;
            offsetX = 0;
            offsetY = 0;

            double targetAspectRatio = (double) wOut / (double) hOut;
            double inputAspectRatio = (double) width / (double) height;

            if ( targetAspectRatio >= inputAspectRatio )
            {
                wIn = wOut;
                hIn = (int) ( (double) height * ( (double) wIn / (double) width ) );
            }
            else
            {
                hIn = hOut;
                wIn = (int) ( (double) width * ( (double) hIn / (double) height ) );
            }
        }

        hr = ScaleWICBitmap( source, wIn > hIn ? wIn : hIn, highQualityScaling );
        if ( FAILED( hr ) )
        {
            printf( "failed to scale input bitmap %#x\n", hr );
            return hr;
        }

        if ( gameBoy )
        {
            hr = ClipWICBitmap( source, wOut, hOut );
            if ( FAILED( hr ) )
            {
                printf( "failed to clip input bitmap: %#x\n", hr );
                return hr;
            }

            wIn = wOut;
            hIn = hOut;

            makeGreyscale = true;
            posterizeLevel = 4;
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
        vector<byte> bufferOut( cbOut );

        if ( 24 == bppOut )
            FloodFill( bufferOut.data(), wOut, hOut, fillColor );
        else
            FloodFill( (USHORT *) bufferOut.data(), wOut, hOut, fillColor );

        hr = DrawImage( bufferOut.data(), strideOut, source, waveMethod, pwcOutput, posterizeLevel,
                        colorizationData, makeGreyscale, offsetX, offsetY, wIn, hIn, bppIn, bppOut );
        if ( FAILED( hr ) )
        {
            printf( "failed to DrawImage %#x\n", hr );
            return hr;
        }

        CTimed writePixels( g_WritePixelsTime );
        hr = bitmapFrameEncode->WritePixels( hOut, strideOut, cbOut, bufferOut.data() );
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

HRESULT StitchImages2( WCHAR const * pwcOutput, CPathArray & pathArray, vector<int> & sortedIndexes,
                       vector<int> & columnsToUse, vector<int> & yOffsets,
                       vector<BitmapDimensions> & dimensions, int columns,
                       int targetHeight, int targetWidth, int spacing, int imageWidth, int fillColor,
                       int posterizeLevel, ColorizationData * colorizationData, bool makeGreyscale,
                       WCHAR const * outputMimetype, bool lowQualityOutput, bool highQualityScaling,
                       bool namesAsCaptions )
{
    CTimed timeStitch( g_CollageStitchTime );
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> bitmapFrameEncode;
    HRESULT hr = CreateWICEncoder( pwcOutput, encoder, bitmapFrameEncode, outputMimetype, lowQualityOutput );
    if ( FAILED( hr ) )
    {
        printf( "can't create a wic encoder: error %#x\n", hr );
        return hr;
    }
    
    hr = bitmapFrameEncode->SetSize( targetWidth, targetHeight );
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
    
    int strideOut = StrideInBytes( targetWidth, g_BitsPerPixel );
    int cbOut = strideOut * targetHeight;
    vector<byte> bufferOut( cbOut );
    
    {
        CTimed timedFlood( g_CollageStitchFloodTime );
    
        FloodFill( bufferOut.data(), targetWidth, targetHeight, fillColor );
    }

    int imageCount = pathArray.Count();

    //for ( int i = 0; i < imageCount; i++ )
    parallel_for( 0, imageCount, [&] ( int i )
    {
        int si = sortedIndexes[ i ];
        int columnToUse = columnsToUse[ si ];
        int imageHeight = round( (double) imageWidth / (double) dimensions[ si ].width * (double) dimensions[ si ].height );

        ComPtr<IWICBitmapSource> source;
        ComPtr<IWICBitmapFrameDecode> frame;
        HRESULT hr = LoadWICBitmap( pathArray[ si ].pwcPath, source, frame, true );
        if ( FAILED( hr ) )
            printf( "can't open bitmap, error: %#x\n", hr );
    
        if ( SUCCEEDED( hr ) )
        {
            hr = ScaleWICBitmap( source, __max( imageWidth, imageHeight ), highQualityScaling );
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
            int xOffset = columnToUse * ( spacing + imageWidth );
            int yOffset = yOffsets[ si ];
            assert( ( yOffset + height ) <= targetHeight );
    
            //printf( "calling DrawImage, xoffset %d, yoffset %d, width %d, height %d\n", xOffset, yOffset, width, height );
        
            DrawImage( bufferOut.data(), strideOut, source, 0, pwcOutput, posterizeLevel, colorizationData, makeGreyscale,
                       xOffset, yOffset, width, height, g_BitsPerPixel, g_BitsPerPixel );

            if ( namesAsCaptions )
                DrawCaption( pathArray[ si ].pwcPath, bufferOut.data(), strideOut, xOffset, yOffset,
                             width, height, targetWidth, targetHeight, g_BitsPerPixel );
        }
    });

    timeStitch.Complete();

    // If the output image is large, most of the time in the app is spent here compressing and writing the image

    CTimed timeWrite( g_CollageWriteTime );

    hr = bitmapFrameEncode->WritePixels( targetHeight, strideOut, cbOut, bufferOut.data() );
    if ( FAILED( hr ) )
    {
        printf( "failed to write pixels %#x\n", hr );
        return hr;
    }

    hr = CommitEncoder( bitmapFrameEncode, encoder );

    return hr;
} //StitchImages2

HRESULT StitchImages1( WCHAR const * pwcOutput, CPathArray & pathArray, vector<BitmapDimensions> & dimensions,
                       int imagesWide, int imagesHigh, int cellDX, int cellDY, int stitchDX, int stitchDY,
                       int fillColor, int waveMethod, int posterizeLevel, ColorizationData * colorizationData,
                       bool makeGreyscale, WCHAR const * outputMimetype, bool lowQualityOutput, bool highQualityScaling,
                       bool namesAsCaptions )
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
    vector<byte> bufferOut( cbOut );
    
    {
        CTimed timedFlood( g_CollageStitchFloodTime );
    
        FloodFill( bufferOut.data(), stitchDX, stitchDY, fillColor );
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
                    hr = ScaleWICBitmap( source, __max( cellDY, cellDX ), highQualityScaling );
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
    
                    DrawImage( bufferOut.data(), strideOut, source, waveMethod, pwcOutput, posterizeLevel, colorizationData,
                               makeGreyscale, rectX, rectY, width, height, g_BitsPerPixel, g_BitsPerPixel );

                    if ( namesAsCaptions )
                        DrawCaption( pathArray[ curSource ].pwcPath, bufferOut.data(), strideOut, xOffset, yOffset,
                                     cellDX, cellDY, stitchDX, stitchDY, g_BitsPerPixel );
                }
            }
        });
    });
    
    timeStitch.Complete();

    // If the output image is large, most of the time in the app is spent here compressing and writing the image

    CTimed timeWrite( g_CollageWriteTime );

    hr = bitmapFrameEncode->WritePixels( stitchDY, strideOut, cbOut, bufferOut.data() );
    if ( FAILED( hr ) )
    {
        printf( "failed to write pixels %#x\n", hr );
        return hr;
    }

    hr = CommitEncoder( bitmapFrameEncode, encoder );

    return hr;
} //StitchImages1

vector<BitmapDimensions> * g_pdimensions = 0;

static int aspectCompare( const void * a, const void * b )
{
    int ia = * (int *) a;
    int ib = * (int *) b;

    vector<BitmapDimensions> & dim = * g_pdimensions;

    double aspecta = (double) dim[ ia ].width / (double) dim[ ia ].height;
    double aspectb = (double) dim[ ib ].width / (double) dim[ ib ].height;

    if ( aspecta > aspectb )
        return 1;

    if ( aspecta < aspectb )
        return -1;

    return 0;
} //aspectCompare

void Randomize( vector<int> & elements, std::mt19937 & gen )
{
    if ( elements.size() <= 1 )
        return;

    std::uniform_int_distribution<> distrib( 0, (int) elements.size() - 1 );

    for ( size_t i = 0; i < elements.size() * 2; i++ )
    {
        int a = distrib( gen );
        int b = distrib( gen );

        swap( elements[ a ], elements[ b ] );
    }
} //Randomize

HRESULT GenerateCollage( int collageMethod, WCHAR * pwcInput, const WCHAR * pwcOutput, int longEdge, int posterizeLevel,
                         ColorizationData * colorizationData, bool makeGreyscale, int collageColumns, int collageSpacing,
                         bool collageSortByAspect, bool collageSpaced, double aspectRatio, int fillColor,
                         WCHAR const * outputMimetype, bool randomizeCollage, bool lowQualityOutput, bool highQualityScaling,
                         bool namesAsCaptions )
{
    CTimed timePrep( g_CollagePrepTime );

    if ( 0.0 == aspectRatio )
        aspectRatio = 1.0;

    CPathArray pathArray;
    WCHAR * pwcDot = wcsrchr( pwcInput, L'.' );
    if ( pwcDot && !wcsicmp( pwcDot, L".txt" ) )
    {
        FILE * fp = _wfopen( pwcInput, L"r" );
        if ( !fp )
        {
            printf( "can't open input file %ws\n", pwcInput );
            exit( 0 );
        }

        char acPath[ MAX_PATH ];
        while ( fgets( acPath, sizeof( acPath ), fp ) )
        {
            char * peol = strchr( acPath, '\r' );
            if ( peol )
                *peol = 0;
            peol = strchr( acPath, '\n' );
            if ( peol )
                *peol = 0;

            if ( 0 != acPath[ 0 ] )
                pathArray.Add( acPath );
        }

        fclose( fp );
    }
    else
    {
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
    
        CEnumFolder enumPaths( false, &pathArray, NULL, 0 );
        enumPaths.Enumerate( awcPath, awcSpec );
    }

    size_t fileCount = pathArray.Count();
    printf( "files found: %zd\n", fileCount );

    if ( 0 == fileCount )
    {
        printf( "no files found in input %ws\n", pwcInput );
        return E_FAIL;
    }

    std::random_device rd;
    std::mt19937 gen( rd() );

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

    if ( 1 == collageMethod )
    {
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
    
        return StitchImages1( pwcOutput, pathArray, dimensions, imagesWide, imagesHigh, minDXEdge, minDYEdge, stitchX, stitchY,
                              fillColor, 0, posterizeLevel, colorizationData, makeGreyscale, outputMimetype, lowQualityOutput, highQualityScaling,
                              namesAsCaptions );
    }

    if ( 2 == collageMethod )
    {
        // Method 2 means a fixed # of columns. Each image is made the same width.

        const int columns = __min( fileCount, collageColumns );
        const int targetWidth = ( 0 == longEdge ) ? 4096 : longEdge;
        const int spacing = collageSpacing; // # of fillColor pixels between images. Not applied to the outside border of the collage

        int imageWidth = ( targetWidth - ( ( columns - 1 ) * spacing ) ) / columns;
        int fullWidth = ( imageWidth * columns ) + ( ( columns - 1 ) * spacing );
        vector<int> columnsToUse( fileCount );
        vector<int> yOffsets( fileCount );
        vector<int> bottoms( fileCount );

        // sort by aspect ratio so the tallest images are placed first. This helps minimize one column being much taller than the others

        vector<int> sortedIndexes( fileCount );
        for ( int i = 0; i < fileCount; i++ )
            sortedIndexes[ i ] = i;

        g_pdimensions = &dimensions;
        qsort( sortedIndexes.data(), fileCount, sizeof( int ), aspectCompare );

        // Lay out the images

        for ( int i = 0; i < fileCount; i++ )
        {
            int si = sortedIndexes[ i ];
            int columnToUse = 0;
            int highestBottom = INT_MAX;
            for ( int c = 0; c < columns; c++ )
            {
                if ( bottoms[ c ] < highestBottom )
                {
                    highestBottom = bottoms[ c ];
                    columnToUse = c;
                }
            }
    
            int imageHeight = round( (double) imageWidth / (double) dimensions[ si ].width * (double) dimensions[ si ].height );
    
            yOffsets[ si ] = bottoms[ columnToUse ];
            bottoms[ columnToUse ] += ( spacing + imageHeight );
            columnsToUse[ si ] = columnToUse;
        }

        // find the tallest column -- that's the collage height

        int fullHeight = 0;
        for ( int c = 0; c < columns; c++ )
        {
            //printf( "height of column %d: %d\n", c, bottoms[ c ] );
            if ( bottoms[ c ] > fullHeight )
                fullHeight = bottoms[ c ];
        }

        fullHeight -= spacing;

        // randomize the vertical position of photos within each column.

        if ( !collageSortByAspect )
        {
            for ( int c = 0; c < columns; c++ )
            {
                // find the indexes of photos in this column
    
                vector<int> randIndex;
                for ( int i = 0; i < fileCount; i++ )
                {
                    int si = sortedIndexes[ i ];
                    if ( columnsToUse[ si ] == c )
                        randIndex.push_back( si );
                }

                Randomize( randIndex, gen );

                // even out the spacing

                int spaceCount = randIndex.size() - 1;
                int extraSpace = fullHeight - ( spaceCount * spacing );
                for ( int i = 0; i < randIndex.size(); i++ )
                {
                    int ri = randIndex[ i ];
                    int imageHeight = round( (double) imageWidth / (double) dimensions[ ri ].width * (double) dimensions[ ri ].height );
                    extraSpace -= imageHeight;
                }

                int extraSpaceBetween = collageSpaced ? spaceCount ? ( extraSpace / spaceCount ) : 0 : 0;
                int extraSpaceLast = collageSpaced ? spaceCount ? ( extraSpace % spaceCount ) : 0 : 0;
                //printf( "extraspace %d, between %d, last %d, spaceCount %d\n", extraSpace, extraSpaceBetween, extraSpaceLast, spaceCount );

                // recompute the y offset of each photo in the column
    
                int currenty = 0;
                for ( int i = 0; i < randIndex.size(); i++ )
                {
                    int ri = randIndex[ i ];

                    yOffsets[ ri ] = currenty;
                    int imageHeight = round( (double) imageWidth / (double) dimensions[ ri ].width * (double) dimensions[ ri ].height );
                    currenty += ( spacing + imageHeight + extraSpaceBetween + ( ( i == spaceCount - 1 ) ? extraSpaceLast : 0 ) );
                }
            }
        }

        timePrep.Complete();

        //printf( "columns %d, target width %d, collage width %d, collage height %d, imageWidth %d\n", columns, targetWidth, fullWidth, fullHeight, imageWidth );

        return StitchImages2( pwcOutput, pathArray, sortedIndexes, columnsToUse, yOffsets, dimensions, columns,
                              fullHeight, fullWidth, spacing, imageWidth, fillColor, posterizeLevel, colorizationData,
                              makeGreyscale, outputMimetype, lowQualityOutput, highQualityScaling,
                              namesAsCaptions );
    }

    return E_FAIL;
} //GenerateCollage

HRESULT ConvertImage( WCHAR const * input, WCHAR const * output, int longEdge, int waveMethod, int posterizeLevel, ColorizationData * colorizationData,
                      bool makeGreyscale, double aspectRatio, int fillColor, WCHAR const * outputMimetype, bool lowQualityOutput, bool gameBoy,
                      bool highQualityScaling )
{
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICBitmapFrameDecode> frame;
    bool force24bppBGR = wcscmp( outputMimetype, L"image/tiff" );

    HRESULT hr = LoadWICBitmap( input, source, frame, force24bppBGR );
    if ( SUCCEEDED( hr ) )
        hr = WriteWICBitmap( output, source, frame, longEdge, waveMethod, posterizeLevel, colorizationData,
                             makeGreyscale, aspectRatio, fillColor, outputMimetype, lowQualityOutput, gameBoy, highQualityScaling );
    
    frame.Reset();
    source.Reset();
    return hr;
} //ConvertImage

void Usage( char * message = 0 )
{
    if ( message )
        printf( "%s\n", message );

    printf( "usage: ic <input> /o:<filename>\n" );
    printf( "  Image Convert\n" );
    printf( "  arguments: <input>           The input image filename. Or for a collage a path specifier or .txt file with image paths\n" );
    printf( "             -a:<aspectratio>  Aspect ratio of output (widthXheight) (e.g. 3x2, 3x4, 16x9, 1x1, 8.51x3.14). Default 1x1 for collages.\n" );
    printf( "             -b                Converts an image into a Game Boy Camera format: 128x112 and 4 shades of grey. Center crop if needed.\n" );
    printf( "             -c                Generates a collage using method 1 (pack images + make square if not all the same aspect ratio.\n" );
    printf( "             -c:1              Same as -c\n" );
    printf( "             -c:2:C:S:A        Generate a collage using method 2 with C fixed-width columns and S pixel spacing. A arrangement (see below)\n" );
    printf( "             -f:<fillcolor>    Color fill for empty space. ARGB or RGB in hex. Default is black.\n" );
    printf( "             -g                Greyscale the output image. Does not apply to the fillcolor.\n" );
    printf( "             -h                Turn off HighQualityCubic scaling and use NearestNeighbor.\n" );
    printf( "             -i                Show CPU and RAM usage.\n" );
    printf( "             -l:<longedge>     Pixel count for the long edge of the output photo or for /c:2 the collage width.\n" );
    printf( "             -o:<filename>     The output filename. Required argument. File will contain no exif info like GPS location.\n" );
    printf( "             -p:x              Posterization level. 1..256 inclusive, Default 0 means none. # colors per channel.\n" );
    printf( "             -q                Sacrifice image quality to produce a smaller JPG output file (4:2:2 not 4:4:4, 60%% not 100%%).\n" );
    printf( "             -r                Randomize the layout of images in a collage.\n" );
    printf( "             -s:x              Clusters color groups and shows most common X colors, Default is 64, 1-256 valid.\n" );
    printf( "             -t                Enable debug tracing to ic.txt. Use -T to start with a fresh ic.txt\n" );
    printf( "             -w:x              Create a WAV file based on the image using methods 1..10. (prototype)\n" );
    printf( "             -zc:x             Colorization. Works like posterization (1-256), but maps to a built-in color table.\n" );
    printf( "             -zc:x,color1,...  Specify x colors that should be used. See example below.\n" );
    printf( "             -zc:x;filename    Use centroids from x color clusters taken from the input file.\n" );
    printf( "             -zb               Same as -zc, but maps colors by matching brightness instead of color.\n" );
    printf( "             -zs               Same as -zc, but maps colors by matching saturation instead of color.\n" );
    printf( "             -zh               Same as -zc, but maps colors by matching hue instead of color.\n" );
    printf( "             -zg               Same as -zc, but maps colors by matching brightness gradient instead of color.\n" );
    printf( "  sample usage: (arguments can use - or /)\n" );
    printf( "    ic picture.jpg /o:newpicture.jpg /l:800\n" );
    printf( "    ic picture.jpg /p o:newpicture.jpg /l:800\n" );
    printf( "    ic picture.jpg /o:c:\\folder\\newpicture.jpg /l:800 /a:1x1\n" );
    printf( "    ic tsuki.tif /o:newpicture.jpg /l:300 /a:1x4 /f:0x003300\n" );
    printf( "    ic miku.tif /o:newpicture.tif /l:300 /a:1x4 /f:2211\n" );
    printf( "    ic phoebe.jpg /o:phoebe_grey.tif /l:3000 /g\n" );
    printf( "    ic julien.jpg /o:julien_grey_posterized.tif /l:3000 /g /p:2 /w:1\n" );
    printf( "    ic picture.jpg /o:newpicture.jpg /l:2000 /a:5.2x3.9\n" );
    printf( "    ic *.jpg /c /o:c:\\collage.jpg /l:2000 /a:5x3 /f:0xff00aa88\n" );
    printf( "    ic images_to_use.txt /c /o:c:\\collage.jpg /l:2000 /a:5x3 /f:0xff00aa88\n" );
    printf( "    ic d:\\pictures\\mitski\\*.jpg /c /o:mitski_collage.jpg /l:10000 /a:4x5\n" );
    printf( "    ic cheekface.jpg /s\n" );
    printf( "    ic cheekface.jpg /s:16 /o:top_16_colors.png\n" );
    printf( "    ic cheekface.jpg /o:cheeckface_colorized.png /zc:3\n" );
    printf( "    ic cheekface.jpg /o:cheeckface_posterized.png /p:8 /g\n" );
    printf( "    ic cfc.jpg /o:out_cfc.png /zc:4,0xfaa616,0x697e94,0xb09e59,0xfdfbe5\n" );
    printf( "    ic cfc.jpg /o:out_cfc.png /zc:16;inputcolors.jpg\n" );
    printf( "    ic cfc.jpg /o:out_cfc.png /zb:64;inputcolors.jpg\n" );
    printf( "    ic cfc.jpg /o:out_cfc.png /zh:8;inputcolors.jpg\n" );
    printf( "    ic /c:2:6:10:S /r /l:4096 d:\\treefort_pics\\*.jpg /o:treefort.png\n" );
    printf( "    ic /c:2:6:10:s /r /l:4096 d:\\treefort_pics\\*.jpg /o:treefort.png\n" );
    printf( "    ic /c:2:6 /o:tf2.png z:\\tf2\\*.jpg /f:eb6145 /l:8192\n" );
    printf( "    ic /i z:\\jbrekkie\\*.jpg /o:michelle_8.png /c:2:5:4:S /zc:8,0xdd9f1a,0xbe812e,0xe3c871,0xe0b74b,0xeee1c1,0xc69948,0x3a3732,0x82543d /f:0xdd9f1a /g\n" );
    printf( "  notes:    - -g only applies to the image, not fillcolor. Use /f with identical rgb values for greyscale fills.\n" );
    printf( "            - Exif data is stripped for your protection.\n" );
    printf( "            - fillcolor is always hex, may or may not start with 0x.\n" );
    printf( "            - Both -a and -l are aspirational for collages. Aspect ratio and long edge may change to accomodate content.\n" );
    printf( "            - If a precise collage aspect ratio or long edge are required, run the app twice; on a single image it's exact.\n" );
    printf( "            - Writes as high a quality of JPG as it can: 1.0 quality and 4:4:4\n" );
    printf( "            - <input> can be any WIC-compatible format: heic, tif, png, bmp, cr2, jpg, etc.\n" );
    printf( "            - Output file is always 24bpp unless both input and output are tif and input is 48bpp, which results in 48bpp.\n" );
    printf( "            - Output file type is inferred from the extension specified. JPG is assumed if not obvious.\n" );
    printf( "            - If an output file is specified with /s, a 128-pixel wide image is created with strips for each color.\n" );
    printf( "            -    collage method 1: -- packs all images of the same aspect ratio or uses squares otherwise.\n" );
    printf( "            -                      -- attempts to match /a: aspect ratio.\n" );
    printf( "            -    collage method 2: -- adds spacing between images and creates identical-width columns.\n" );
    printf( "            -                      -- The longedge argument applies to the width, which may be shorter than the height.\n" );
    printf( "            -                      -- defaults are 3 columns, 6 pixels of spacing, and don't sort by aspect ratio (-c:2:3:6:n).\n" );
    printf( "            -                      -- doesn't attempt to match /a: aspect ratio since a column count is specified.\n" );
    printf( "            -                      -- /A arrangement arguments - uppercase yes, lowercase no\n" );
    printf( "            -                         T (tallest items on top) / t (random arrangement (default))\n" );
    printf( "            -                         S (space images out (default)) / s (force consistent spacing and perhaps leave blank space at bottom\n" );
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

static DWORD ColorizationColors2[] =
{
    0xbfbbaf, 0x905c54,
};

static DWORD ColorizationColors4[] =
{
    0xddd0bd, 0xe38853, 0x7099bf, 0x5b4644,
};

static DWORD ColorizationColors8[] =
{
    0xeea665, 0xeecea1, 0x797874, 0x97b9c6, 0x473d35, 0xd55838, 0xdadce3, 0x4b8edd,
};

static DWORD ColorizationColors16[] =
{
    0xeda48c, 0xf3dea5, 0x5b4f4d, 0x90a093, 0xe8e4e3, 0x7a766e, 0xe67a69, 0x99c4e6,
    0x61a3e5, 0xf2673e, 0xc0bcb3, 0x342724, 0xb83d2e, 0x4379cf, 0xf0d94d, 0x308c36,
};

static DWORD ColorizationColors32[] =
{
    0xee9b81, 0xf5b1a4, 0xf9785d, 0xf5e8a0, 0xebd8ce, 0xf25d32, 0xf0ede9, 0x9bbde3,
    0x6b6b6e, 0x89878e, 0x433939, 0xa8a49b, 0xc2bfb8, 0xfbd96b, 0x4e525d, 0x734b41,
    0xd27469, 0xb4d8f2, 0x261a18, 0xc7301b, 0x7fa0d6, 0x3d9ae8, 0x5c84c9, 0x67b7f2,
    0x948061, 0xb14d52, 0xf2d026, 0x3069d2, 0x78c196, 0x389964, 0x179e14, 0xfd89fc,
};

static DWORD ColorizationColors64[] =
{
    0xf88f7f, 0xf2a89e, 0xf67464, 0xf6be7c, 0xf0d6a5, 0xf85c43, 0x4f4538, 0xf5e8c8,
    0x312f31, 0x60645d, 0x9d9593, 0x8d817b, 0x444f55, 0xc7bdb4, 0xf7bfc2, 0xf5f1eb,
    0xeca354, 0xaba5a9, 0xe94d25, 0x715448, 0x221817, 0xf6f4a1, 0x747272, 0xd9d3ce,
    0xb22b25, 0xd07a77, 0xdb9590, 0xf6f174, 0xe3e6ea, 0xd35c58, 0x718292, 0x92b6c9,
    0x6d9abb, 0x477ed0, 0xa94c52, 0x4e9bed, 0xafc6d6, 0xf6e04c, 0x6eaeec, 0x93b8ed,
    0x51667b, 0xc2ddf2, 0x7c9be2, 0x946553, 0x627ad2, 0xe78721, 0x7cc5f2, 0xc0ad82,
    0x74b986, 0x3862c4, 0xacd7f8, 0x2b99ee, 0xb49555, 0x9fd3ee, 0xd8310d, 0xf8e819,
    0x782f2e, 0x92ce9b, 0x359f77, 0x52c8ee, 0x1c71db, 0x1b7d1c, 0x12a70d, 0x06fffe,
};

static DWORD ColorizationColors128[] =
{
    0xf58374, 0xf9958c, 0xf9a5a1, 0xf86a62, 0xeea775, 0xaea8a8, 0xf9ecb3, 0x63676b,
    0x79848d, 0xbebab8, 0xf85c43, 0xa89b91, 0xf68757, 0xf6be8c, 0x626058, 0x907c72,
    0xf4f5f2, 0xfacc6e, 0xf9d9cb, 0x4d4f50, 0xf64b2f, 0x948e8c, 0x77757a, 0xf6f179,
    0x493f41, 0x5d4e37, 0x6cabf2, 0xcfc7c9, 0xefac53, 0xdd8d8a, 0xfbea50, 0xda756e,
    0x909aa4, 0xf6f599, 0xd45e59, 0xf7d1ae, 0x7d6c5a, 0xf2e3e3, 0xfad899, 0xf7bfc2,
    0xf8f4d3, 0xedb4ad, 0xe04f16, 0xe1d8d8, 0x5699ea, 0xf66d3f, 0x73524e, 0x96b9f2,
    0xe0ebf5, 0xdfa19d, 0x4a3b28, 0x83bef6, 0xb6cadb, 0xab271a, 0xa0bbd5, 0x363336,
    0x95acc7, 0x6a8fe0, 0xa96163, 0x302827, 0xcc7a7d, 0xb4d8f2, 0x4c7edd, 0xcfdbe6,
    0x5d6dcf, 0xbe4a52, 0xe2dd9f, 0xbe3431, 0x3671d9, 0x46556c, 0x6d97bf, 0x7eaedc,
    0x5b748a, 0xdb2d0b, 0x4fb5f7, 0x7ec190, 0x849be7, 0x558bbb, 0xe1d6ba, 0xc5b281,
    0x963e4f, 0x96d2f7, 0xcec49e, 0xbfe3fb, 0xa4976b, 0xfac52b, 0x201c16, 0xa6c9f3,
    0x329cf2, 0xf3f21b, 0x6ab282, 0x334b57, 0x91d3da, 0x6b8ba9, 0xd08143, 0x7f6532,
    0x599160, 0x3556c1, 0xe38e2b, 0x3fa8d8, 0x78373d, 0x2289e6, 0x0f0a07, 0x993f27,
    0x2a364c, 0x1b2429, 0xf48407, 0x41200d, 0x75c6b8, 0xa57a3d, 0x2a110d, 0xaad4ae,
    0x37af82, 0x386eb4, 0x5dcfea, 0x7e2217, 0xb5ab3c, 0x3a796d, 0x179e14, 0x98d58e,
    0x19957e, 0x2e9b2a, 0x3b5789, 0x0f7b0e, 0xe63636, 0x0856f4, 0xfd89fc, 0x06fffe,
};

static DWORD ColorizationColors256[] =
{
    0xf86b55, 0x948e8c, 0xf9958a, 0xf87a6d, 0xfaf0b5, 0xfadbcb, 0xf7f3e8, 0x86817d,
    0xfc5b49, 0x4d464d, 0xf8f4d3, 0x626260, 0x585759, 0xd76e6c, 0xe0d7d2, 0x49423b,
    0xa59c8b, 0xc7bcbe, 0xaba9ae, 0xfb6941, 0x5d636f, 0xa2a39f, 0x8ec3f3, 0x4fb5f7,
    0xe9dfe1, 0x5b4a3b, 0x716a69, 0xe3f1f2, 0xf6f5f9, 0xfae49e, 0xe48074, 0xf88c5e,
    0x6ea6f3, 0xf88b78, 0xf8a398, 0x7ba2e7, 0xda5e5a, 0xf77c82, 0x40352c, 0xf9e0de,
    0x2e2b2d, 0xf4c090, 0xf24b3b, 0xe48b89, 0xf2f66d, 0xfbd8ae, 0xeca17e, 0xfbd776,
    0x51a1de, 0xc35b5c, 0xf8fa8c, 0xbcd9f6, 0x90969f, 0x84c28e, 0x727079, 0xf5c8cd,
    0xfbf59f, 0x3b3538, 0xfab07f, 0x635148, 0xd29292, 0xfac3bf, 0x735754, 0x947f6e,
    0xb7a69d, 0x475669, 0xe04519, 0xd68278, 0xe79b91, 0xfe8949, 0xabc6db, 0xccbf94,
    0x828490, 0xd2551d, 0xf2c8a6, 0x507dd8, 0x96a4b4, 0xbec4c9, 0xe7e1a5, 0xf4be5e,
    0xeeafb2, 0xf6bd78, 0xf9ada8, 0xaa3e21, 0x4081e7, 0xcb322d, 0x7b8c96, 0xfacf84,
    0xa3b6c5, 0x5290f3, 0xd8d2b5, 0x88adce, 0xfa5630, 0xcfdce8, 0x4e8bce, 0xb61f16,
    0xd4c7c5, 0x70b27e, 0xe82708, 0xa8d0f6, 0xe88554, 0x271d1c, 0x8b7275, 0x689ddf,
    0xbdaa7d, 0xf93c1e, 0x8395ad, 0x96b9d6, 0x7f695b, 0xd2dddb, 0x3a6edb, 0xae464f,
    0x362722, 0xf6ce9b, 0xe7a05e, 0xf99399, 0xf45c6f, 0xdfd69b, 0xf5ab57, 0xdfa19d,
    0x3b9df4, 0x75c6b8, 0x3c4e5c, 0xbdd2e0, 0xeda86c, 0xaaa168, 0xf0ec81, 0xeb6236,
    0x618be6, 0x7bbef3, 0x2c63ca, 0x6493bc, 0x91aeef, 0xefd5be, 0xb3b5b7, 0x9cbeeb,
    0xac8683, 0xc9484f, 0xcc7a7d, 0x994141, 0x866239, 0x727c8b, 0x9b5561, 0x82bee2,
    0x94d5fe, 0x6d4245, 0xf9ca32, 0x313c4f, 0xbcb4a9, 0xb4696d, 0xb6302f, 0x827761,
    0x698bae, 0xf8fb4c, 0x4f4220, 0xefaf99, 0x523237, 0x566d7d, 0xa5bbd8, 0xd1eafb,
    0x248bf4, 0xd2c8d1, 0x3289d3, 0x4f67c7, 0xafdefe, 0xe87927, 0xf8ea18, 0xfcb99f,
    0x1a73de, 0xfaef35, 0xe4bcbb, 0x4b84b5, 0xd47e36, 0xfeda5d, 0x66bae8, 0xb88653,
    0xf99aa4, 0x788ddc, 0x687ed3, 0x5db98f, 0x34b07b, 0x6c693c, 0xaedceb, 0x6ec5ff,
    0xef9c3d, 0xf2571e, 0x6c8194, 0x9d6355, 0xf6dd53, 0xcab6ab, 0xfbe909, 0x7b3128,
    0x75a0c3, 0xf9acc1, 0x29363f, 0x645423, 0xe9ebc5, 0x93d3e0, 0x96b9fd, 0x495c54,
    0x8ecfc6, 0x60758d, 0xb05f3b, 0xdfb1ad, 0xebf199, 0x8a4452, 0x3b200f, 0x5d9d68,
    0x18a108, 0x261012, 0x3c8967, 0x666cda, 0x98d58e, 0x8f2b14, 0x95894d, 0x3e5682,
    0x171717, 0x1d232f, 0xaeab40, 0x20676b, 0x3556c1, 0xf67800, 0xaad4ae, 0xf6b712,
    0x0d0d0f, 0x648359, 0xcbc563, 0x19957e, 0x8f96e7, 0x3c130f, 0x5d9954, 0x090504,
    0x842640, 0x378e2b, 0x37b3d9, 0x210909, 0x691d29, 0x4d799e, 0x3562a6, 0x037602,
    0x51e5ef, 0xb1bdef, 0xc5b627, 0x29554a, 0x281b08, 0xfd89fc, 0x06fffe, 0x182a14,
    0x232c67, 0x034afe, 0x187218, 0x7fffff, 0x00a637, 0xcfcaff, 0x8bfe02, 0xf0335b,
};

extern "C" int wmain( int argc, WCHAR * argv[] )
{
    #ifndef NDEBUG
        parallel_for ( 0, 5, [&] ( int testing )
        {
            assert( KDTreeBGR::UnitTest() );
        } );
    #endif

    long long totalTime = 0;
    CTimed timedTotal( totalTime );

    if ( argc < 2 )
        Usage( "too few arguments" );

    HRESULT hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if ( FAILED( hr ) )
    {
        printf( "can't initialize COM: %#x\n", hr );
        Usage();
    }

    hr = CoCreateInstance( CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                           __uuidof( IWICImagingFactory ),
                           (void **) g_IWICFactory.GetAddressOf() );
    if ( FAILED( hr ) )
    {
        printf( "can't create WIC imaging factory: %#x\n", hr );
        Usage();
    }

    static WCHAR awcInput[ MAX_PATH ] = {0};
    static WCHAR awcOutput[ MAX_PATH ] = {0};
    bool gameBoy = false;
    bool generateCollage = false;
    int collageMethod = 1;
    int collageColumns = 3;
    int collageSpacing = 6;
    bool collageSortByAspect = false;
    bool collageSpaced = true;
    bool namesAsCaptions = false;
    bool randomizeCollage = false;
    bool runtimeInfo = false;
    bool highQualityScaling = true;
    bool showColors = false;
    int showColorCount = 64;
    bool makeGreyscale = false;
    bool lowQualityOutput = false;
    int posterizeLevel = 0;  // 0 means none
    ColorizationData * colorizationData = 0; // null means none
    int waveMethod = 0;      // 0 means none; don't create a WAV file
    int longEdge = 0;
    int fillColor = 0xff << 24; // black, non-transparent
    double aspectRatio = 0.0;
    int breakTileSize = 0; // if 0, don't do it.
    double tileProbability = 1.0;
    bool enableTracing = false;
    bool clearTraceFile = false;

    ColorizationData cd;

    for ( int a = 1; a < argc; a++ )
    {
        WCHAR const * parg = argv[ a ];

        if ( L'-' == parg[0] || L'/' == parg[0] )
        {
            WCHAR p = tolower( parg[1] );

            if ( L'a' == p )
            {
                if ( L':' != parg[2] )
                    Usage( "malformed argument -- expecting a :" );

                aspectRatio = ParseAspectRatio( parg + 3 );
            }
            else if ( L'b' == p )
                gameBoy = true;
            else if ( L'c' == p )
            {
                generateCollage = true;

                if ( ':' == parg[ 2 ] && 0 != parg[ 3 ] )
                {
                    collageMethod = _wtoi( parg + 3 );

                    if ( collageMethod < 1 || collageMethod > 2 )
                        Usage( "collage method isn't valid" );

                    if ( 2 == collageMethod )
                    {
                        WCHAR const * pwcColon1 = wcschr( parg + 4, ':' );
                        WCHAR const * pwcColon2 = ( 0 != pwcColon1 ) ? wcschr( pwcColon1 + 1, ':' ) : 0;
                        WCHAR const * pwcColon3 = ( 0 != pwcColon2 ) ? wcschr( pwcColon2 + 1, ':' ) : 0;

                        if ( 0 != pwcColon1 )
                            collageColumns = _wtoi( pwcColon1 + 1 );

                        if ( 0 != pwcColon2 )
                            collageSpacing = _wtoi( pwcColon2 + 1 );

                        if ( 0 != pwcColon3 )
                        {
                            for ( const WCHAR * pwcA = pwcColon3 + 1; *pwcA; pwcA++ )
                            {
                                if ( 'T' == *pwcA )
                                    collageSortByAspect = true;
                                else if ( 't' == *pwcA )
                                    collageSortByAspect = false;
                                else if ( 'S' == *pwcA )
                                    collageSpaced = true;
                                else if ( 's' == *pwcA )
                                    collageSpaced = false;
                                else
                                    Usage( "invalid collage A argument" );
                            }
                        }

                        if ( collageColumns < 1 || collageColumns > 100 )
                            Usage( "invalid collage column count" );

                        if ( collageSpacing < 0 || collageSpacing > 100 )
                            Usage( "invalid collage spacing" );
                    }
                }
            }
            else if ( L'f' == p )
            {
                if ( L':' != parg[2] )
                    Usage( "malformed argument -- expecting a :" );

               int parsed = swscanf_s( parg + 3, L"%x", & fillColor );

               if ( 0 == parsed )
                   Usage( "can't parse fill color" );
            }
            else if ( L'g' == p )
                makeGreyscale = true;
            else if ( L'h' == p )
                highQualityScaling = false;
            else if ( L'i' == p )
                runtimeInfo = true;
            else if ( L'l' == p )
            {
                if ( L':' != parg[2] )
                    Usage( "malformed argument -- expecting a :" );

                longEdge = _wtoi( parg + 3 );

                if ( longEdge <= 0 )
                {
                    printf( "long edge -l is invalid: %d\n", longEdge );
                    Usage();
                }
            }
            else if ( L'n' == p )
                namesAsCaptions = true;
            else if ( L'o' == p )
            {
                if ( L':' != parg[2] )
                    Usage( "malformed argument -- expecting a :" );

                _wfullpath( awcOutput, parg + 3, _countof( awcOutput ) );
            }
            else if ( L'p' == p )
            {
                if ( L':' != parg[2] )
                    Usage( "malformed argument -- expecting a :" );

                posterizeLevel = _wtoi( parg + 3 );
                if ( posterizeLevel < 1 || posterizeLevel > 256 )
                {
                    printf( "invalid posterization level %d; must be 1..256\n", posterizeLevel );
                    Usage();
                }
            }
            else if ( L'q' == p )
                lowQualityOutput = true;
            else if ( L'r' == p )
                randomizeCollage = true;
            else if ( L's' == p )
            {
                showColors = true;

                if ( L':' == parg[2] )
                    showColorCount = _wtoi( parg + 3 );

                if ( showColorCount < 0 || showColorCount > 256 )
                    Usage( "show color count must be in range 1..256" );
            }
            else if ( L't' == p )
            {
                if ( 0 != parg[2] )
                    Usage( "unexpected characters after argument" );
                enableTracing = true;
                clearTraceFile = ( L'T' == parg[1] );
            }
            else if ( L'w' == p )
            {
                if ( L':' != parg[2] )
                    Usage( "malformed argument -- expecting a :" );

                waveMethod = _wtoi( parg + 3 );
                if ( waveMethod < 0 || waveMethod > 10 )
                {
                    printf( "invalid wave method %d\n", waveMethod );
                    Usage();
                }
            }
            else if ( L'z' == p )
            {
                WCHAR const * pnext = parg + 2;
                if ( L'b' == *pnext )
                    cd.mapping = mapBrightness;
                else if ( 's' == *pnext )
                    cd.mapping = mapSaturation;
                else if ( 'h' == *pnext )
                    cd.mapping = mapHue;
                else if ( 'g' == *pnext )
                    cd.mapping = mapGradient;
                else if ( 'c' == *pnext )
                    cd.mapping = mapColor;
                else
                    Usage( "invalid /z flag specified" );

                pnext++;
                if ( L':' != *pnext )
                    Usage( "colon not found in /z flag" );

                posterizeLevel = _wtoi( pnext + 1 );
                if ( posterizeLevel < 1 || posterizeLevel > 256 )
                {
                    printf( "invalid colorization posterization level %d; must be 1-256\n", posterizeLevel );
                    Usage();
                }

                colorizationData = &cd;

                WCHAR const *semi = wcschr( parg, L';' );
                if ( semi )
                {
                    static WCHAR awcColorFile[ MAX_PATH ] = {0};
                    _wfullpath( awcColorFile, semi + 1, _countof( awcColorFile ) );
                    DWORD attr = GetFileAttributesW( awcColorFile );
                    if ( INVALID_FILE_ATTRIBUTES == attr )
                        Usage( "can't find /z color file" );

                    cd.bgrdata.clear();
                    ShowColors( awcColorFile, posterizeLevel, cd.bgrdata, false, 0 );
                }
                else
                {
                    WCHAR const *comma = wcschr( parg, L',' );
    
                    if ( comma )
                    {
                        // parse a list of colors

                        cd.bgrdata.clear();
    
                        do
                        {
                            comma++;
                            DWORD color;
                            int parsed = swscanf_s( comma, L"%x", & color );
                            if ( 0 == parsed )
                                Usage( "can't parse color mapping color" );
                            cd.bgrdata.push_back( color );
                            comma = wcschr( comma, L',' );
                        } while ( comma );
    
                        if ( cd.bgrdata.size() != posterizeLevel )
                            Usage( "the /z: color count isn't the same as the number of colors specified" );
                    }
                    else
                    {
                        // use the built-in table

                        int countBuiltIn = 0;
                        DWORD * pBuiltInArray = NULL;

                        if ( posterizeLevel <= 2 )
                        {
                            countBuiltIn = 2;
                            pBuiltInArray = ColorizationColors2;
                        }
                        else if ( posterizeLevel <= 4 )
                        {
                            countBuiltIn = 4;
                            pBuiltInArray = ColorizationColors4;
                        }
                        else if ( posterizeLevel <= 8 )
                        {
                            countBuiltIn = 8;
                            pBuiltInArray = ColorizationColors8;
                        }
                        else if ( posterizeLevel <= 16 )
                        {
                            countBuiltIn = 16;
                            pBuiltInArray = ColorizationColors16;
                        }
                        else if ( posterizeLevel <= 32 )
                        {
                            countBuiltIn = 32;
                            pBuiltInArray = ColorizationColors32;
                        }
                        else if ( posterizeLevel <= 64 )
                        {
                            countBuiltIn = 64;
                            pBuiltInArray = ColorizationColors64;
                        }
                        else if ( posterizeLevel <= 128 )
                        {
                            countBuiltIn = 128;
                            pBuiltInArray = ColorizationColors128;
                        }
                        else // anything greater is mapped to 256
                        {
                            countBuiltIn = 256;
                            pBuiltInArray = ColorizationColors256;
                        }

                        cd.bgrdata.resize( __min( posterizeLevel, countBuiltIn ) );

                        for ( int c = 0; c < cd.bgrdata.size(); c++ )
                           cd.bgrdata[ c ] = pBuiltInArray[ c ];
                    }
                }
            }
        }
        else if ( 0 != awcInput[0] )
            Usage( "input file specified twice" );
        else
            _wfullpath( awcInput, parg, _countof( awcInput ) );
    }

    tracer.Enable( enableTracing, L"ic.txt", clearTraceFile );

    ULONG_PTR gdiplusToken = 0;

    if ( namesAsCaptions )
    {
        GdiplusStartupInput si;
        GdiplusStartup( &gdiplusToken, &si, NULL );
    }

    // Create optimized data structures for specific color mapping scenarios

    if ( mapColor == cd.mapping || mapGradient == cd.mapping )
        qsort( cd.bgrdata.data(), cd.bgrdata.size(), sizeof DWORD, compare_brightness );

    if ( mapColor == cd.mapping )
    {
        cd.kdtree.reset( new KDTreeBGR( cd.bgrdata.size() ) );

        for ( int i = 0; i < cd.bgrdata.size(); i++ )
            cd.kdtree->Insert( cd.bgrdata[ i ] );
    }
    else  if ( mapBrightness == cd.mapping || mapHue == cd.mapping || mapSaturation == cd.mapping )
    {
        qsort( cd.bgrdata.data(), cd.bgrdata.size(), sizeof DWORD,
               mapHue == cd.mapping ? compare_hue :
               mapSaturation == cd.mapping ? compare_saturation :
               compare_brightness );

        // calcuate the HSV data for the rgb data, if any
    
        cd.hsvdata.resize( cd.bgrdata.size() );
    
        for ( int i = 0; i < cd.bgrdata.size(); i++ )
        {
            int h,s,v;
            BGRToHSV( cd.bgrdata[ i ], h, s, v );

            if ( mapHue == cd.mapping )
                cd.hsvdata[ i ] = h;
            else if ( mapSaturation == cd.mapping )
                cd.hsvdata[ i ] = s;
            else if ( mapBrightness == cd.mapping )
                cd.hsvdata[ i ] = v;
        }

        #ifndef NDEBUG
        for ( int z = 0; z < cd.hsvdata.size() - 1; z++ )
            assert( cd.hsvdata[ z ] <= cd.hsvdata[ z + 1 ] );
        #endif
    }

    if ( 0 == awcInput[0] || ( 0 == awcOutput[0] && !showColors ) )
        Usage( "input and/or output files not specified" );

    if ( waveMethod > 0 && generateCollage )
        Usage( "can't generate wav files when generating a collage" );

    if ( !generateCollage )
    {
        DWORD attr = GetFileAttributesW( awcInput );
        if ( INVALID_FILE_ATTRIBUTES == attr )
        {
            printf( "can't open file %ws\n", awcInput );
            Usage();
        }
    }

    const WCHAR * outputMimetype = awcOutput[0] ? InferOutputType( PathFindExtension( awcOutput ) ) : 0;

    tracer.Trace( "input: %ws\n", awcInput );
    tracer.Trace( "output: %ws\n", awcOutput );
    tracer.Trace( "output type: %ws\n", outputMimetype );
    tracer.Trace( "long edge: %d\n", longEdge );

    if ( showColors )
    {
        cd.bgrdata.clear();
        hr = ShowColors( awcInput, showColorCount, cd.bgrdata, true, awcOutput[0] ? awcOutput : 0, outputMimetype );
    }
    else if ( generateCollage )
    {
        hr = GenerateCollage( collageMethod, awcInput, awcOutput, longEdge, posterizeLevel, colorizationData, makeGreyscale,
                              collageColumns, collageSpacing, collageSortByAspect, collageSpaced, aspectRatio, fillColor,
                              outputMimetype, randomizeCollage, lowQualityOutput, highQualityScaling, namesAsCaptions );
        if ( SUCCEEDED( hr ) )
            printf( "collage written successfully: %ws\n", awcOutput );
        else
            DeleteFile( awcOutput );
    }
    else
    {
        hr = ConvertImage( awcInput, awcOutput, longEdge, waveMethod, posterizeLevel, colorizationData, makeGreyscale,
                           aspectRatio, fillColor, outputMimetype, lowQualityOutput, gameBoy, highQualityScaling );
        if ( SUCCEEDED( hr ) )
            printf( "output written successfully: %ws\n", awcOutput );
        else
        {
            printf( "conversion of image failed with error %#x\n", hr );
            DeleteFile( awcOutput );
        }
    }

    if ( gdiplusToken )
        GdiplusShutdown( gdiplusToken );

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

        if ( 0 != g_ShowColorsAllTime )
        {
            PrintStat( "show colors total:", g_ShowColorsAllTime / CTimed::NanoPerMilli() );

            if ( 0 != g_ShowColorsOpenTime )
                PrintStat( "  open:", g_ShowColorsOpenTime / CTimed::NanoPerMilli() );

            if ( 0 != g_ShowReadPixelsTime )
                PrintStat( "  copy pixels:", g_ShowReadPixelsTime / CTimed::NanoPerMilli() );
            
            PrintStat( "  copy colors", g_ShowColorsCopyTime / CTimed::NanoPerMilli() );
            PrintStat( "  sort:", g_ShowColorsSortTime / CTimed::NanoPerMilli() );
            PrintStat( "  find unique:", g_ShowColorsUniqueTime / CTimed::NanoPerMilli() );
            PrintStat( "  feature selection:", g_ShowColorsClusterFeatureSelectionTime / CTimed::NanoPerMilli() );

            if ( 0 != g_ShowColorsFeaturizeClusterTime );
                PrintStat( "  featurization:", g_ShowColorsFeaturizeClusterTime / CTimed::NanoPerMilli() );

            if ( 0 != g_ShowColorsClusterRunTime );
                PrintStat( "  cluster runtime:", g_ShowColorsClusterRunTime / CTimed::NanoPerMilli() );

            if ( 0 != g_ShowColorsPostClusterTime );
                PrintStat( "  post-processing:", g_ShowColorsPostClusterTime / CTimed::NanoPerMilli() );

            if ( 0 != g_ShowColorsPaletteTime )
                PrintStat( "  palette file:", g_ShowColorsPaletteTime / CTimed::NanoPerMilli() );
        }

        if ( generateCollage )
        {
            PrintStat( "collage prep:", g_CollagePrepTime / CTimed::NanoPerMilli() );
            PrintStat( "collage stitch:", g_CollageStitchTime / CTimed::NanoPerMilli() );
            PrintStat( "  flood fill:", g_CollageStitchFloodTime / CTimed::NanoPerMilli() );
            PrintStat( "  read pixels:", g_CollageStitchReadPixelsTime / CTimed::NanoPerMilli() );
            PrintStat( "  draw:", g_CollageStitchDrawTime / CTimed::NanoPerMilli() );
            PrintStat( "collage write:", g_CollageWriteTime / CTimed::NanoPerMilli() );
        }

        if ( 0 != g_ReadPixelsTime )
            PrintStat( "read pixels:", g_ReadPixelsTime / CTimed::NanoPerMilli() );

        if ( 0 !=  g_PosterizePixelsTime )
            PrintStat( "posterize image:", g_PosterizePixelsTime / CTimed::NanoPerMilli() );

        if ( 0 != g_ColorizeImageTime )
            PrintStat( "colorize image:", g_ColorizeImageTime / CTimed::NanoPerMilli() );

        if ( 0 != g_WritePixelsTime )
            PrintStat( "write pixels:", g_WritePixelsTime / CTimed::NanoPerMilli() );

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
