#pragma once

// Minimally parse and read uncompressed WAV files. Only supports formats I could test.
// WAV file writing support is started but far from complete.

#include <dshow.h>
#include <dmo.h>
#include <mmreg.h>

#pragma comment( lib, "strmiids.lib" )

#include <assert.h>
#include <djltrace.hxx>
#include <djl_strm.hxx>

class DjlParseWav
{
    public:

        #pragma pack( push, 1 )
        
            struct WavHeader
            {
                DWORD riff;               // "RIFF"
                DWORD size;               // size of file minus 8 bytes
                DWORD wave;               // "WAVE"
            };

            struct WavChunkHeader
            {
                DWORD format;             // e.g. "fmt ", "JUNK", "bext", "data", etc.
                DWORD formatSize;         // size of format data beyond this struct before the next chunk
            };
               
            struct WavSubchunk : WavChunkHeader
            {
                WORD  formatType;         // 1 == PCM, 3 == IEEE float, 6 == 8-bit ITU-T G.711 A-law, 7 = 8-bit ITU-T G.711 æ-law, 0xfffe == extensible
                WORD  channels;           // count of channels
                DWORD sampleRate;         // e.g. 44100 samples per second
                DWORD dataRate;           // ( sampleRate * bitsPerSample * Channels ) / 8
                WORD  blockAlign;         // number of bytes for one sample including all channels
                WORD  bitsPerSample;      // probably 8, 16, 24, 32, or 64
                WORD  cbExtension;        // optional; # of bytes that follow in this struct
                WORD  validBits;          // # of valid bits
                DWORD channelMask;        // speaker position mask
                GUID  subFormat;          // the extended format of the data

                WavSubchunk( WORD type, WORD chans, DWORD srate, WORD align, WORD bps )
                {
                    ZeroMemory( this, sizeof WavSubchunk );
                    memcpy( &format, "fmt ", 4 );
                    formatSize = sizeof WavSubchunk - 8;
                    formatType = type;
                    channels = chans;
                    sampleRate = srate;
                    blockAlign = align;
                    bitsPerSample = bps;
                    cbExtension = 0;
                    dataRate = sampleRate * (DWORD) bitsPerSample / 8;
                }

                WavSubchunk()
                {
                    ZeroMemory( this, sizeof WavSubchunk );
                }
            };

            struct WavInfochunk : WavChunkHeader
            {
                // ISFT == Name of the software package used to create the file
                // 49 4e 46 4f 49 53 46 54 0e 00 00 00 64 61 76 69 64 6c 79 2e 30 31 2e 30 31 00
                // INFOISFT....davidly.01.01.
                byte info[26] = { 0x49, 0x4e, 0x46, 0x4f, 0x49, 0x53, 0x46, 0x54, 0x0e, 0x00, 0x00, 0x00, 0x64,
                                  0x61, 0x76, 0x69, 0x64, 0x6c, 0x79, 0x2e, 0x30, 0x31, 0x2e, 0x30, 0x31, 0x00 };

                void Init()
                {
                    memcpy( &format, "LIST", 4 );
                    formatSize = sizeof( info );
                }
            };
        
        #pragma pack( pop )

        DjlParseWav( WCHAR const * pwcFile ) :
            stream( pwcFile ),
            successfulParse( false ),
            samples( 0 ),
            bytesPS( 0 ),
            sampleRate( 0.0 ),
            forWrite( false ),
            fmtType( 0 )
        {
            if ( stream.Ok() )
            {
                successfulParse = parseStream( stream );
                stream.CloseFile();
            }
        } //DjlParseWav

        DjlParseWav( WCHAR const * pwcFile, WavSubchunk & wavsub ) :
            stream( pwcFile, true ),
            successfulParse( false ),
            samples( 0 ),
            bytesPS( 0 ),
            sampleRate( 0.0 ),
            forWrite( true ),
            fmtType( 0 )
        {
            fmtSubchunk = wavsub;
            sampleRate = (double) fmtSubchunk.sampleRate;
            bytesPS = fmtSubchunk.bitsPerSample / 8;

            if ( 0 == wavsub.channels ||
                 0 == wavsub.dataRate ||
                 ( wavsub.bitsPerSample != 8 && wavsub.bitsPerSample != 16 && wavsub.bitsPerSample != 24 && wavsub.bitsPerSample != 32) ||
                 ( wavsub.blockAlign != ( wavsub.channels * bytesPS ) ) )
            {
                tracer.Trace( "malformed WavSubchunk header\n" );
                return;
            }

            tracer.TraceDebug( !stream.Ok(), "DjlParseWav unable to open file %ws\n", pwcFile );
        } //DjlParseWav

        // Instantiate an in-memory wave file for reading

        DjlParseWav( const WAVEFORMATEX * wf, byte * buffer, size_t size ) :
            bytesPS( 0 ),
            fmtType( 0 ),
            forWrite( false ),
            sampleRate( 0.0 ),
            samples( 0 ),
            successfulParse( false )
        {
            // wf may actually be a WAVEFORMATEXTENSIBLE, and that's fine.

            if ( ( sizeof wf + wf->cbSize ) > sizeof WAVEFORMATEXTENSIBLE )
            {
                tracer.Trace( "malformed wafeformat -- it's too large to be a WAVEFORMATEXTENSIBLE\n" );
                return;
            }

            successfulParse = true;
            memcpy( &fmtSubchunk.formatType, wf, sizeof WAVEFORMATEX + wf->cbSize );
            samples = (DWORD) ( size / ( fmtSubchunk.bitsPerSample / 8 ) / fmtSubchunk.channels );
            sampleRate = (double) fmtSubchunk.sampleRate;
            bytesPS = fmtSubchunk.bitsPerSample / 8;
            fmtType = fmtSubchunk.formatType;

            data.reset( new byte[ size ] );
            memcpy( data.get(), buffer, size );
        } //DjlParseWav

        bool SuccessfulParse() { return successfulParse; }
        bool OpenSuccessful() { return SuccessfulParse() || stream.Ok(); }
        WavSubchunk & GetFmt() { return fmtSubchunk; }
        const byte * GetData() { return data.get(); }
        DWORD Samples() { return samples; }
        WORD Channels() { return fmtSubchunk.channels; }
        double SecondsOfSound() { return (double) samples / sampleRate; }

        const WCHAR * GetFormatType()
        {
            if ( 1 == fmtType )
                return L"PCM";
            if ( 3 == fmtType )
                return L"ieee float";
            if ( 6 == fmtType )
                return L"8-bit A-law";
            if ( 7 == fmtType )
                return L"8-bit Mu-law";
            if ( 0xfffe == fmtType )
                return L"extensible";

            return L"unknown";
        } //GetFormatType

        void GetSample( DWORD s, double & left, double & right )
        {
            assert( s < samples );
            assert( fmtSubchunk.channels > 1 );

            left = GetChannel( s, 0 );
            assert( left >= -1.0 );
            assert( left <= 1.0 );

            right = GetChannel( s, 1 );
            assert( right >= -1.0 );
            assert( right <= 1.0 );
        } //GetSample

        double GetSampleLeft( DWORD s )
        {
            assert( s < samples );

            double left = GetChannel( s, 0 );
            assert( left >= -1.0 );
            assert( left <= 1.0 );
            return left;
        } //GetSample

        double GetSampleInChannel( DWORD s, DWORD channel )
        {
            assert( s < samples );
            assert( channel < fmtSubchunk.channels );

            double left = GetChannel( s, channel );
            assert( left >= -1.0 );
            assert( left <= 1.0 );
            return left;
        } //GetSampleInChannel

        double GetSampleRight( DWORD s )
        {
            assert( s < samples );
            assert( fmtSubchunk.channels > 1 );

            double right = GetChannel( s, 1 );
            assert( right >= -1.0 );
            assert( right <= 1.0 );
            return right;
        } //GetSampleRight

        bool WriteWavFile( byte * pdata, ULONG bytesData )
        {
            WavHeader wh;
            memcpy( &wh.riff, "RIFF", 4 );
            wh.size = ( sizeof WavHeader + sizeof WavSubchunk + sizeof WavInfochunk + sizeof WavChunkHeader + bytesData ) - 8; // size of the file - 8;
            memcpy( &wh.wave, "WAVE", 4 );
            stream.Write( &wh, sizeof wh );

            stream.Write( &fmtSubchunk, sizeof fmtSubchunk );

            WavInfochunk whInfo;
            whInfo.Init();
            stream.Write( &whInfo, sizeof whInfo );

            WavChunkHeader dataChunkOut;
            memcpy( &dataChunkOut.format, "data", 4 );
            dataChunkOut.formatSize = bytesData;
            stream.Write( &dataChunkOut, sizeof dataChunkOut );
            stream.Write( pdata, bytesData );

            return true;
        } //WriteWavFile

        static void WriteSample( byte * pdata, int index, double left, double right, int bps, int blockAlign, WORD formatType = 1 )
        {
            int offset = index * blockAlign;
            pdata += offset;

            if ( 8 == bps && 1 == formatType )
            {
                char l = (char) round( left * (double) 0x7f );
                char r = (char) round( right * (double) 0x7f );

                *pdata++ = l;
                *pdata = r;
            }
            else if ( 16 == bps && 1 == formatType )
            {
                short l = (short) round( left * (double) 0x7fff );
                short r = (short) round( right * (double) 0x7fff );

                memcpy( pdata, &l, sizeof l );
                memcpy( pdata + ( bps / 8 ), &r, sizeof r );
            }
            else if ( 24 == bps && 1 == formatType )
            {
                int32_t l = (int32_t) round( left * (double) 0x7fffff );
                int32_t r = (int32_t) round( right * (double) 0x7fffff );

                memcpy( pdata, &l, 3 );
                memcpy( pdata + 3, &r, 3 );
            }
            else if ( 32 == bps && 3 == formatType )
            {
                float fl = (float) left;
                float fr = (float) right;

                memcpy( pdata, &fl, sizeof fl );
                memcpy( pdata + 4, &fr, sizeof fl );
            }
            else
            {
                tracer.Trace( "unsupported bps %d and formatType in WriteSample\n", bps, formatType );
            }
        } //WriteSample

        void OverwriteSample( int index, double v, DWORD channel )
        {
            DWORD chOffset = channel * bytesPS;
            DWORD offset = chOffset + ( index * fmtSubchunk.blockAlign );
            byte *pdata = data.get() + offset;

            if ( 1 == bytesPS && 1 == fmtType )
            {
                char x = (char) round( v * (double) 0x7f );
                *pdata = x;
            }
            else if ( 2 == bytesPS && 1 == fmtType )
            {
                short x = (short) round( v * (double) 0x7fff );
                memcpy( pdata, &x, sizeof x );
            }
            else if ( 3 == bytesPS && 1 == fmtType )
            {
                int32_t x = (int32_t) round( v * (double) 0x7fffff );
                memcpy( pdata, &x, 3 );
            }
            else if ( 4 == bytesPS  && 3 == fmtType )
            {
                float x = (float) v;
                memcpy( pdata, &x, sizeof x );
            }
            else
            {
                tracer.Trace( "unsupported bytesPS %d and formatType in OverwriteSample\n", bytesPS, fmtType );
            }
        } //OverwriteSample

        double Sample( double s, DWORD maxSamples = 128, WORD channel = 0 )
        {
            assert( successfulParse );
            assert( channel < fmtSubchunk.channels );

            // s is in radians but may outside the range of 0 .. ( 2 * PI ). In this case, modulus it

            const double TwoPI = 2.0 * 3.141592653589793238462643383279502884197169399;

            s = fmod( s, TwoPI );

            if ( s < 0.0 )
                s = TwoPI + s;

            // map the range 0 to TwoPI to the # of samples to get a sample index

            DWORD usedSamples = __min( maxSamples, samples );
            DWORD index = (DWORD) ( s / TwoPI * (double) usedSamples );

            assert( index < samples );

            double d = GetChannel( index, channel );
            assert( d <= 1.0 );
            assert( d >= -1.0 );

            return d;
        } //Sample

        double Wave( double noteTime, double noteMultiplier, WORD channel = 0 )
        {
            assert( successfulParse );
            assert( channel < fmtSubchunk.channels );
            DWORD index = (DWORD) ( noteTime * sampleRate * noteMultiplier );

            // it's not a bug -- it's if we're past the end of the waveform

            if ( index >= samples )
                return 0.0;

            assert( index < samples );

            double d = GetChannel( index, channel );

            assert( d <= 1.0 );
            assert( d >= -1.0 );

            //tracer.Trace( "sample time %lf, index %d (of %d), sample %lf\n", noteTime, index, samples, d );

            return d;
        } //Wave

        void Normalize( double amount = 0.70794578 )
        {
            if ( 0 == samples )
                return;

            if ( amount > 1.0 || amount <= 0.0 )
                return;

            // set the maximum volume to the amount specified. -3db or about .70795 is normal

            double maxSample = 0.0;

            for ( DWORD s = 0; s < samples; s++ )
            {
                for ( int c = 0; c < fmtSubchunk.channels; c++ )
                {
                    double d = fabs( GetSampleInChannel( s, c ) );
                    if ( d > maxSample )
                        maxSample = d;
                }
            }

            double factor = amount / maxSample;

            for ( DWORD s = 0; s < samples; s++ )
            {
                for ( int c = 0; c < fmtSubchunk.channels; c++ )
                {
                    double d = factor * GetSampleInChannel( s, c );
                    OverwriteSample( s, d, c );
                }
            }
        } //Normalize

        void Reverse()
        {
            if ( 0 == samples )
                return;

            DWORD top = 0;
            DWORD bottom = samples - 1;

            while ( top < bottom )
            {
                for ( int c = 0; c < fmtSubchunk.channels; c++ )
                {
                    double t = GetSampleInChannel( top, c );
                    double b = GetSampleInChannel( bottom, c );

                    OverwriteSample( top, b, c );
                    OverwriteSample( bottom, t, c );
                }

                top++;
                bottom--;
            }
        } //Reverse

    private:

        CStream stream;
        bool successfulParse;
        unique_ptr<byte> data;
        WavSubchunk fmtSubchunk;
        DWORD samples;
        int bytesPS;
        double sampleRate;
        bool forWrite;
        WORD fmtType;

        void TraceGuid( GUID & guid )
        {
            tracer.TraceQuiet( "Guid = {%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
                               guid.Data1, guid.Data2, guid.Data3,
                               guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                               guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7] );
        } //TraceGuid

        bool parseStream( CStream & stream )
        {
            WavHeader header;
        
            __int64 len = stream.Length();
            if ( len < sizeof header )
            {
                tracer.Trace( "Wav file is too small\n" );
                return false;
            }
        
            stream.GetBytes( 0, &header, sizeof header );
        
            if ( memcmp( & header.riff, "RIFF", sizeof header.riff ) )
            {
                tracer.Trace( "Wav header isn't RIFF\n" );
                return false;
            }
        
            if ( memcmp( & header.wave, "WAVE", sizeof header.wave ) )
            {
                tracer.Trace( "Wav wave header isn't WAVE\n" );
                return false;
            }

            __int64 offset = sizeof header;
        
            while ( offset < len )
            {
                WavSubchunk chunk;
                stream.GetBytes( offset, &chunk, sizeof chunk );
        
                //printf( "chunk: %c%c%c%c, size %d\n", chunk.format & 0xff, ( chunk.format >> 8 ) & 0xff,
                //                                      ( chunk.format >> 16 ) & 0xff, ( chunk.format >> 24 ) & 0xff,
                //                                      chunk.formatSize );
        
                if ( !memcmp( &chunk.format, "fmt ", 4 ) )
                {
                    //printf( "  type of format:  %d\n", chunk.formatType );
                    //printf( "  channels:        %d\n", chunk.channels );
                    //printf( "  sample rate:     %d\n", chunk.sampleRate );
                    //printf( "  dataRate:        %d\n", chunk.dataRate );
                    //printf( "  block align:     %d\n", chunk.blockAlign );
                    //printf( "  bits per sample: %d\n", chunk.bitsPerSample );
        
                    fmtSubchunk = chunk;
                    fmtType = fmtSubchunk.formatType;
                    sampleRate = (double) chunk.sampleRate;
                }
                else if ( !memcmp( &chunk.format, "data", 4 ) )
                {
                    if ( 0xfffe == fmtType )
                    {
                        tracer.Trace( "extensible format; " );
                        TraceGuid( fmtSubchunk.subFormat );
                        tracer.TraceQuiet( "\n" );

                        #if false
                        MEDIASUBTYPE_PCM
                        MEDIASUBTYPE_IEEE_FLOAT
                        MEDIASUBTYPE_PCM_FL64
                        MEDIASUBTYPE_PCM_FL64_le
                        MEDIASUBTYPE_PCM_FL32
                        MEDIASUBTYPE_PCM_FL32_le
                        MEDIASUBTYPE_PCM_IN32
                        MEDIASUBTYPE_PCM_IN32_le
                        MEDIASUBTYPE_PCM_IN24
                        MEDIASUBTYPE_PCM_IN24_le
                        MFAudioFormat_PCM_HDCP
                        #endif                               
                    }

                    if ( 1 != fmtType && 3 != fmtType && 6 != fmtType && 7 != fmtType && 0xfffe != fmtType )
                    {
                        tracer.Trace( "format type %#x isn't supported 1/3/6/7/Ext are supported)\n", fmtType );
                        return false;
                    }

                    int bps = fmtSubchunk.bitsPerSample;

                    if ( ( 0 == fmtSubchunk.channels ) ||
                         ( 0 == fmtSubchunk.dataRate ) ||
                         ( 8 != bps && 16 != bps && 24 != bps && 32 != bps && 64 != bps ) )
                    {
                        tracer.Trace( "unsupported channels %d, datarate %d, or bits per sample %d\n", fmtSubchunk.channels, fmtSubchunk.dataRate, bps );
                        return false;
                    }

                    bytesPS = bps / 8;
                    int bytesPerSample =  fmtSubchunk.channels * ( fmtSubchunk.bitsPerSample / 8 );

                    if ( bytesPerSample > fmtSubchunk.blockAlign )
                    {
                        tracer.Trace( "bytes per sample (%d) > block align (%d); malformed WAV file\n", bytesPerSample, fmtSubchunk.blockAlign );
                        return false;
                    }

                    samples = chunk.formatSize / fmtSubchunk.blockAlign;

                    if ( stream.Length() < ( offset + 8 + chunk.formatSize ) )
                    {
                        tracer.Trace( "stream length %lld isn't long enough for implied size %d\n", stream.Length(), ( offset + 8 + chunk.formatSize ) );
                        return false;
                    }

                    //printf( "seconds of sound: %lf\n", (double) samples / sampleRate );

                    data.reset( new byte[ chunk.formatSize ] );
                    stream.GetBytes( offset + 8, data.get(), chunk.formatSize );
        
                    return true; // don't worry about later chunks
                }
        
                offset += ( (__int64) chunk.formatSize + (__int64) 8 );
            }
        
            return false;
        } //parseStream

        double GetExtendedChannel( DWORD offset )
        {
            if ( 0xfffe != fmtType )
                return 0.0;

            if ( MEDIASUBTYPE_IEEE_FLOAT == fmtSubchunk.subFormat )
            {
                if ( 8 == bytesPS )
                {
                    assert( 8 == sizeof( double ) );
                    double d;
                    memcpy( &d, data.get() + offset, sizeof d );
                    return d;
                }
                else if ( 4 == bytesPS )
                {
                    assert( 4 == sizeof( float ) );
                    float f;
                    memcpy( &f, data.get() + offset, sizeof f );

                    // WAV files created with Scarlett hardware and Windows APIs result in slightly out of bounds values

                    if ( f > 1.0 )
                        f = 1.0;
                    else if ( f < -1.0 )
                        f = -1.0;

                    return (double) f;
                }
                else
                    tracer.Trace( "unexpected bytesPS %d in GetExtendedChannel IEEE_FLOAT\n" );
            }

            if ( MEDIASUBTYPE_PCM == fmtSubchunk.subFormat )
            {
                if ( 4 == bytesPS )
                {
                    long l;
                    memcpy( &l, data.get() + offset, sizeof l );
                    return (double) l / (double) (long) 0x7fffffff;
                }
                else
                    tracer.Trace( "unexpected bytesPS %d in GetExtendedChannel PCM\n" );
            }

            return 0.0;
        } //GetExtendedChannel

        __forceinline double GetChannel( DWORD index, int channel )
        {
            assert( index < samples );

            DWORD chOffset = channel * bytesPS;
            DWORD offset = chOffset + ( index * fmtSubchunk.blockAlign );

            if ( 2 == bytesPS && 1 == fmtType )
            {
                byte *p = data.get() + offset;
                int32_t v = *p | ( *(p+1) << 8 );

                // sign extend from 16 bits to 32 bits

                if ( 0x8000 & v )
                    v |= 0xffff0000;

                tracer.TraceDebug( v > 32767, "v: %#x\n", v );
                assert( v <= 32767 );
                assert( v >= -32768 );

                return (double) v / (double) 32768;
            }

            if ( 3 == bytesPS && 1 == fmtType )
            {
                byte *p = data.get() + offset;
                int32_t v = *p | ( *(p+1) << 8 ) | ( *(p+2) << 16 );
    
                // sign extend from 24 bits to 32 bits
    
                if ( v & 0x00800000 )
                    v |= 0xff000000;
    
                const int32_t denom = ( 1 << 23 );
                //tracer.Trace( "denom: %d, v: %d, %#x\n", denom, v, v );
    
                assert( v <= ( denom - 1 ) );
                assert( v >= - denom );
                
                return (double) v / (double) denom;
            }

            if ( 4 == bytesPS && 3 == fmtType )
            {
                float f = * (float *) ( data.get() + offset );
                //tracer.Trace( "float read from file: %f\n", f );
                return (double) f;
            }

            if ( 1 == bytesPS && 1 == fmtType )
            {
                int32_t v = (int) ( * (char *) ( data.get() + offset ) );

                // sign extend from 8 bits to 32 bits

                if ( 0x80 & v )
                    v |= 0xffffff00;

                assert( v <= 127 );
                assert( v >= -128 );

                return (double) v / (double) 128;
            }

            if ( 1 == bytesPS && 6 == fmtType )
                return (double) ALawDecompressTable[ * ( data.get() + offset ) ] / 32768.0;

            if ( 1 == bytesPS && 7 == fmtType )
                return (double) MuLawDecompressTable[ * ( data.get() + offset ) ] / 32768.0;

            if ( 0xfffe == fmtType )
                return GetExtendedChannel( offset );

            return 0.0;
        } //GetChannel

        // There are dozens of copies of these tables on the internet and I'm not sure of the origin.

        const short MuLawDecompressTable[256] =
        {
             -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
             -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
             -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
             -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
              -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
              -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
              -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
              -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
              -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
              -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
               -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
               -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
               -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
               -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
               -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
                -56,   -48,   -40,   -32,   -24,   -16,    -8,    -1,
              32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
              23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
              15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
              11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
               7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
               5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
               3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
               2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
               1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
               1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
                876,   844,   812,   780,   748,   716,   684,   652,
                620,   588,   556,   524,   492,   460,   428,   396,
                372,   356,   340,   324,   308,   292,   276,   260,
                244,   228,   212,   196,   180,   164,   148,   132,
                120,   112,   104,    96,    88,    80,    72,    64,
                 56,    48,    40,    32,    24,    16,     8,     0
        };
        
        const short ALawDecompressTable[256] =
        {
             -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
             -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
             -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
             -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
             -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,
             -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
             -11008,-10496,-12032,-11520,-8960, -8448, -9984, -9472,
             -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
             -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296,
             -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424,
             -88,   -72,   -120,  -104,  -24,   -8,    -56,   -40,
             -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168,
             -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
             -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
             -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592,
             -944,  -912,  -1008, -976,  -816,  -784,  -880,  -848,
              5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736,
              7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784,
              2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368,
              3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392,
              22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
              30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
              11008, 10496, 12032, 11520, 8960,  8448,  9984,  9472,
              15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
              344,   328,   376,   360,   280,   264,   312,   296,
              472,   456,   504,   488,   408,   392,   440,   424,
              88,    72,   120,   104,    24,     8,    56,    40,
              216,   200,   248,   232,   152,   136,   184,   168,
              1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184,
              1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696,
              688,   656,   752,   720,   560,   528,   624,   592,
              944,   912,  1008,   976,   816,   784,   880,   848
        };
}; //DjlParseWav
    


