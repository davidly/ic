# IC
### Image Conversion command-line app for Windows

Useful for converting from one format to another (e.g. heic to jpg) or creating a lower-resolution version of an image. The app copies no EXIF data like GPS location to the output image. The app also creates simple collages of images.

The app uses k-means clustering for finding most common colors in images and a kd-tree for color mapping. The most common colors are written to stdout, but also an image can be created with a colorstrip and hex codes for each color.

After converting to "Game Boy Camera" format, you may want to scale up the bitmap to a more visible size while turning off interpolation. That
way the "pixels" are more visible. For example:

    ic d:\pictures\sasquatch_2018\batch3\soccer_mommy.png /b /o:sm.png
    ic sm.png /l:512 /o:sm512.png -h

To build on Windows:
    
    Using a Visual Studio x64 Native Tools Command Prompt window, run m.bat

usage: ic input /o:output
    
    Image Convert
    arguments: <input>           The input image filename. Or for a collage a path specifier or .txt file with image paths
             -a:<aspectratio>  Aspect ratio of output (widthXheight) (e.g. 3x2, 3x4, 16x9, 1x1, 8.51x3.14). Default 1x1 for collages.
             -b                Converts an image into a Game Boy Camera format: 128x112 and 4 shades of grey. Center crop if needed.
             -c                Generates a collage using method 1 (pack images + make square if not all the same aspect ratio.
             -c:1:C            Same as -c but also sorts images in the collage based on their primary color.
             -c:2:C:S:A        Generate a collage using method 2 with C fixed-width columns and S pixel spacing. A arrangement (see below)
             -f:<fillcolor>    Color fill for empty space. ARGB or RGB in hex. Default is black.
             -g                Greyscale the output image. Does not apply to the fillcolor.
             -h                Turn off HighQualityCubic scaling and use NearestNeighbor.
             -i                Show CPU and RAM usage.
             -l:<longedge>     Pixel count for the long edge of the output photo or for /c:2 the collage width.
             -n                Use filenames as captions in collages.
             -o:<filename>     The output filename. Required argument. File will contain no exif info like GPS location.
             -p:x              Posterization level. 1..256 inclusive, Default 0 means none. # colors per channel.
             -q                Sacrifice image quality to produce a smaller JPG output file (4:2:2 not 4:4:4, 60% not 100%).
             -r                Randomize the layout of images in a collage.
             -s:x              Clusters color groups and shows most common X colors, Default is 64, 1-256 valid.
             -t                Enable debug tracing to ic.txt. Use -T to start with a fresh ic.txt
             -w:x              Create a WAV file based on the image using methods 1..10. (prototype)
             -zc:x             Colorization. Works like posterization (1-256), but maps to a built-in color table.
             -zc:x,color1,...  Specify x colors that should be used. See example below.
             -zc:x;filename    Use centroids from x color clusters taken from the input file.
             -zb               Same as -zc, but maps colors by matching brightness instead of color.
             -zs               Same as -zc, but maps colors by matching saturation instead of color.
             -zh               Same as -zc, but maps colors by matching hue instead of color.
             -zg               Same as -zc, but maps colors by matching brightness gradient instead of color.
    
    sample usage: (arguments can use - or /)
      ic picture.jpg /o:newpicture.jpg /l:800
      ic picture.jpg /p o:newpicture.jpg /l:800
      ic picture.jpg /o:c:\folder\newpicture.jpg /l:800 /a:1x1
      ic tsuki.tif /o:newpicture.jpg /l:300 /a:1x4 /f:0x003300
      ic miku.tif /o:newpicture.tif /l:300 /a:1x4 /f:2211
      ic phoebe.jpg /o:phoebe_grey.tif /l:3000 /g
      ic julien.jpg /o:julien_grey_posterized.tif /l:3000 /g /p:2 /w:1
      ic picture.jpg /o:newpicture.jpg /l:2000 /a:5.2x3.9
      ic *.jpg /c /o:c:\collage.jpg /l:2000 /a:5x3 /f:0xff00aa88
      ic d:\pictures\mitski\*.jpg /c /o:mitski_collage.jpg /l:10000 /a:4x5
      ic cheekface.jpg /s
      ic cheekface.jpg /s:16 /o:top_16_colors.png
      ic cheekface.jpg /o:cheeckface_colorized.png /zc:3
      ic cheekface.jpg /o:cheeckface_posterized.png /p:8 /g
      ic cfc.jpg /o:out_cfc.png /zc:4,0xfaa616,0x697e94,0xb09e59,0xfdfbe5
      ic cfc.jpg /o:out_cfc.png /zc:16;inputcolors.jpg
      ic cfc.jpg /o:out_cfc.png /zb:64;inputcolors.jpg
      ic cfc.jpg /o:out_cfc.png /zh:8;inputcolors.jpg
      ic /c:2:6:10:S /r /l:4096 d:\treefort_pics\*.jpg /o:treefort.png
      ic /c:2:6:10:s /r /l:4096 d:\treefort_pics\*.jpg /o:treefort.png
      ic /i z:\jbrekkie\*.jpg /o:michelle_8.png /c:2:5:4:S /zc:8,0xdd9f1a,0xbe812e,0xe3c871,0xe0b74b,0xeee1c1,0xc69948,0x3a3732,0x82543d /f:0xdd9f1a /g
    notes:    - -g only applies to the image, not fillcolor. Use /f with identical rgb values for greyscale fills.
              - Exif data is stripped for your protection.
              - fillcolor is always hex, may or may not start with 0x.
              - Both -a and -l are aspirational for collages. Aspect ratio and long edge may change to accomodate content.
              - If a precise collage aspect ratio or long edge are required, run the app twice; on a single image it's exact.
              - Writes as high a quality of JPG as it can: 1.0 quality and 4:4:4
              - <input> can be any WIC-compatible format: heic, tif, png, bmp, cr2, jpg, etc.
              - Output file is always 24bpp unless both input and output are tif and input is 48bpp, which results in 48bpp.
              - Output file type is inferred from the extension specified. JPG is assumed if not obvious.
              - If an output file is specified with /s, a 128-pixel wide image is created with strips for each color.
              -    collage method 1: -- packs all images of the same aspect ratio or uses squares otherwise.
              -                      -- attempts to match /a: aspect ratio.
              -    collage method 2: -- adds spacing between images and creates identical-width columns.
              -                      -- The longedge argument applies to the width, which may be shorter than the height.
              -                      -- defaults are 3 columns, 6 pixels of spacing, and don't sort by aspect ratio (-c:2:3:6:n).
              -                      -- doesn't attempt to match /a: aspect ratio since a column count is specified.
              -                      -- /A arguments - uppercase yes, lowercase no
              -                         T (tallest items on top) / t (random arrangement (default))
              -                         S (space images out (default)) / s (force consistent spacing and perhaps leave blank space at bottom

