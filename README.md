# IC
### Image Conversion command-line app for Windows

Useful for converting from one format to another (e.g. heic to jpg) or creating a lower-resolution version of an image. The app copies no EXIF data like GPS data to the output image. The app also creates simple collages of images.

To build on Windows:
    
    Using a Visual Studio x64 Native Tools Command Prompt window, run m.bat

usage: ic input /o:output
    
    Image Convert
    
    arguments: <input>           The input image filename or path specifier for a collage.
               -a:<aspectratio>  Aspect ratio of output (widthXheight) (e.g. 3x2, 3x4, 16x9, 1x1, 8.51x3.14). 
               -c                Generate a collage. <input> is a path to input files. Aspect ratio attempted, not guaranteed.
               -f:<fillcolor>    Color fill for empty space. ARGB or RGB in hex. Default is black.
               -g                Greyscale the output image. Does not apply to the fillcolor.
               -i                Show CPU and RAM usage.
               -l:<longedge>     Pixel count for the long edge of the output photo.
               -o:<output>       The output filename. Required argument. File will contain no exif info like GPS location.
               -p:x              Posterization level. 0..35 inclusive, where 0 means none. Default is 0.
               -q                Sacrifice image quality to produce a smaller JPG output file (4:2:2 not 4:4:4, 60% not 100%).
               -r                Randomize the layout of images in a collage.
               -t                Enable debug tracing to ic.txt. Use -T to start with a fresh ic.txt
               -w:x              Create a WAV file based on the image using methods 1..10. (prototype)
  
  sample usage: (arguments can use - or /)
    
    ic picture.jpg /o:newpicture.jpg /l:800
    ic picture.jpg /p o:newpicture.jpg /l:800
    ic picture.jpg /o:c:\folder\newpicture.jpg /l:800 /a:1x1
    ic tsuki.tif /o:newpicture.jpg /l:300 /a:1x4 /f:0x003300
    ic miku.tif /o:newpicture.tif /l:300 /a:1x4 /f:2211
    ic phoebe.jpg /o:phoebe_grey.tif /l:3000 /g
    ic julien.jpg /o:julien_grey_posterized.tif /l:3000 /g /p:1 /w:1
    ic picture.jpg /o:newpicture.jpg /l:2000 /a:5.2x3.9
    ic *.jpg /c /o:c:\collage.jpg /l:2000 /a:5x3 /f:0xff00aa88
    ic d:\pictures\mitski\*.jpg /c /o:mitski_collage.jpg /l:10000 /a:4x5
    
  notes: 
    
    - -g only applies to the image, not fillcolor. Use /f with identical rgb values for greyscale fills.
    - Exif data is stripped for your protection.
    - fillcolor may or may not start with 0x.
    - Both -a and -l are aspirational for collages. Aspect ratio and long edge may change to accomodate content.
    - If a precise collage aspect ratio or long edge are required, run the app twice; on a single image it's exact.
    - Writes as high a quality of JPG as it can: 1.0 quality and 4:4:4
    - <input> can be any WIC-compatible format: heic, tif, png, bmp, cr2, jpg, etc.
    - Output file is always 24bpp unless both input and output are tif and input is 48bpp, which results in 48bpp.
    - Output file type is inferred from the extension specified. JPG is assumed if not obvious.

