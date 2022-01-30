#pragma once

//
// Stores a list of colors and finds the best matching one for arbitrary input colors.
// Useful for mapping colors in an image to a palette (perhaps from another image).
// This is about the same speed as a linear search for 16 colors. It's slower for
// fewer colors and much faster for more colors. It's 4x faster for 256 colors.
// Only supports up to 64k - 1 colors, but could be extended by expanding ushort variables.
//
// This class assumes colors are stored BGR (B is at the lowest address). But it should work
// with any color ordering provided you're consistent on input/output.
//

#include <stdlib.h>
#include <vector>

class KDTreeBGR
{
    private:

        typedef unsigned short ushort;

        struct KDNode
        {
            ushort left, right;
            byte R, G, B;

            int Distance( int r, int g, int b )
            {
                int distR = (int) R - r;
                int distG = (int) G - g;
                int distB = (int) B - b;
                return distR * distR + distG * distG + distB * distB;
            }
        }; //KDNode   
        
        struct RectRGB
        {
            byte minR, minG, minB, filler1, maxR, maxG, maxB, filler2;
        
            void SetInfinite()
            {
                minR = 0;
                minG = 0;
                minB = 0;
        
                maxR = 255;
                maxG = 255;
                maxB = 255;
            }
        }; //RectRGB

        struct SearchState
        {
            int targetR, targetG, targetB;
            int bestDistanceSq, best;
        };

        vector<KDNode> nodeArray;
        ushort treeHead;
        ushort nodesAllocated;

        int GoDeep( int node, int level )
        {
            int ldepth = level;
    
            if ( 0 != nodeArray[ node ].left )
                 ldepth = GoDeep( nodeArray[ node ].left, level + 1 );
    
            int rdepth = level;
    
            if ( 0 != nodeArray[ node ].right )
                rdepth = GoDeep( nodeArray[ node ].right, level + 1 );
    
            return __max( ldepth, rdepth );
        } //GoDeep

        ushort Insert( int r, int g, int b, ushort t, int level )
        {
            if ( 0 == t )
            {
                t = nodesAllocated;
                nodesAllocated++;
    
                nodeArray[ t ].R = (byte) r;
                nodeArray[ t ].G = (byte) g;
                nodeArray[ t ].B = (byte) b;
            }
            else
            {
                assert( ( r != nodeArray[ t ].R ) || ( g != nodeArray[ t ].G ) || ( b != nodeArray[ t ].B ) );
                assert( level <= 2 );
    
                bool goRight;
    
                if ( 0 == level )
                    goRight = ( r > nodeArray[ t ].R );
                else if ( 1 == level )
                    goRight = ( g > nodeArray[ t ].G );
                else
                    goRight = ( b > nodeArray[ t ].B );
    
                if ( goRight )
                    nodeArray[ t ].right = Insert( r, g, b, nodeArray[ t ].right, ( level + 1 ) % 3 );
                else
                    nodeArray[ t ].left = Insert( r, g, b, nodeArray[ t ].left, ( level + 1 ) % 3 );
            }
    
            return t;
        } //Insert
    
        void NearestNeighbor( ushort kd, RectRGB & leftRect, int level, SearchState & ss )
        {
            //printf( "nearest neighbor %d, level %d\n", kd, level );
    
            KDNode & kdn = nodeArray[ kd ];
            int kdToTarget = kdn.Distance( ss.targetR, ss.targetG, ss.targetB );
    
            //printf( "  comparing target %#.2x%.2x%.2x to node %#.2x%.2x%.2x, distance %d\n", targetB, targetG, targetR, kdn.B, kdn.G, kdn.R, kdToTarget );

            if ( kdToTarget < ss.bestDistanceSq )
            {
                ss.best = kd;
                ss.bestDistanceSq = kdToTarget;
            }
    
            if ( 0 == ( kdn.left | kdn.right ) ) // no more work for leaf nodes
                return;
            
            int s = level % 3;
            bool targetInLeft;
            RectRGB rightRect = leftRect;
    
            if ( 0 == s )
            {
                leftRect.maxR = kdn.R;
                rightRect.minR = kdn.R;
                targetInLeft = ( ss.targetR < kdn.R );
            }
            else if ( 1 == s )
            {
                leftRect.maxG = kdn.G;
                rightRect.minG = kdn.G;
                targetInLeft = ( ss.targetG < kdn.G );
            }
            else
            {
                leftRect.maxB = kdn.B;
                rightRect.minB = kdn.B;
                targetInLeft = ( ss.targetB < kdn.B );
            }
    
            if ( targetInLeft )
            {
                //printf( "  target is on left, left: %d\n", kdn.left );
                if ( 0 != kdn.left )
                    NearestNeighbor( kdn.left, leftRect, level + 1, ss );
    
                if ( 0 != kdn.right )
                {
                    int f = ( ss.targetR > rightRect.minR ) ? ( ss.targetR > rightRect.maxR ) ? rightRect.maxR : ss.targetR : rightRect.minR;
                    int diff = f - (int) ss.targetR;
                    int sqrDistance = diff * diff;
    
                    f = ( ss.targetG > rightRect.minG ) ? ( ss.targetG > rightRect.maxG ) ? rightRect.maxG : ss.targetG : rightRect.minG;
                    diff = f - (int) ss.targetG;
                    sqrDistance += diff * diff;
    
                    f = ( ss.targetB > rightRect.minB ) ? ( ss.targetB > rightRect.maxB ) ? rightRect.maxB : ss.targetB : rightRect.minB;
                    diff = f - (int) ss.targetB;
                    sqrDistance += diff * diff;
    
                    if ( sqrDistance < ss.bestDistanceSq )
                        NearestNeighbor( kdn.right, rightRect, level + 1, ss );
                }        
            }
            else
            {
                //printf( "  target is on right, right: %d\n", kdn.right );
                if ( 0 != kdn.right )
                    NearestNeighbor( kdn.right, rightRect, level + 1, ss );
    
                if ( 0 != kdn.left )
                {
                    int f = ( ss.targetR > leftRect.minR ) ? ( ss.targetR > leftRect.maxR ) ? leftRect.maxR : ss.targetR : leftRect.minR;
                    int diff = f - (int) ss.targetR;
                    int sqrDistance = diff * diff;
    
                    f = ( ss.targetG > leftRect.minG ) ? ( ss.targetG > leftRect.maxG ) ? leftRect.maxG : ss.targetG : leftRect.minG;
                    diff = f - (int) ss.targetG;
                    sqrDistance += diff * diff;
    
                    f = ( ss.targetB > leftRect.minB ) ? ( ss.targetB > leftRect.maxB ) ? leftRect.maxB : ss.targetB : leftRect.minB;
                    diff = f - (int) ss.targetB;
                    sqrDistance += diff * diff;
    
                    if ( sqrDistance < ss.bestDistanceSq )
                        NearestNeighbor( kdn.left, leftRect, level + 1, ss );
                }           
            }
        } //NearestNeighbor

    public:
    
        KDTreeBGR( int count )
        {
            assert( count < 65535 );
    
            // Reserve node 0 as a "null" value. Utilized array indexes begin at 1.
    
            nodeArray.resize( count + 1 );
            ZeroMemory( nodeArray.data(), ( count + 1 ) * sizeof KDNode );
            treeHead = 0;
            nodesAllocated = 1;
        } //KDTreeBGR

        int NodeCount()
        {
            return nodesAllocated - 1; // the 0th element is reserved
        } //NodeCount
    
        int Depth()
        {
            if ( 0 == treeHead )
                return 0;
    
            return GoDeep( treeHead, 1 );
        } //Depth
    
        void Insert( int c )
        {
            int b = (byte) ( c & 0xff );
            int g = (byte) ( ( c >> 8 ) & 0xff );
            int r = (byte) ( ( c >> 16 ) & 0xff );
            Insert( r, g, b );
        } //Insert
    
        void Insert( int r, int g, int b )
        {
            treeHead = Insert( r, g, b, treeHead, 0 );
        } //Insert

        int Nearest( int r, int g, int b, DWORD & id )
        {
            SearchState ss;
            ss.targetR = r;
            ss.targetG = g;
            ss.targetB = b;
            ss.bestDistanceSq = INT_MAX;
            ss.best = 0;
    
            RectRGB leftRect;
            leftRect.SetInfinite();
            NearestNeighbor( treeHead, leftRect, 0, ss );

            // id returns the 0-based index of the best matching color based on the order in
            // in which colors were added to the tree. Since slot 0 is unused, subtract 1.

            assert( 0 != ss.best );
            id = ss.best - 1;
            return ( (DWORD) nodeArray[ ss.best ].R << 16 ) | ( (DWORD) nodeArray[ ss.best ].G << 8 ) | ( (DWORD) nodeArray[ ss.best ].B );
        } //Nearest
    
        int Nearest( int c, DWORD & id )
        {
            byte b = (byte) ( c & 0xff );
            byte g = (byte) ( ( c >> 8 ) & 0xff );
            byte r = (byte) ( ( c >> 16 ) & 0xff );
            return Nearest( r, g, b, id );
        } //Nearest

        void ShowTree()
        {
            printf( "tree head: %d, nodesallocated %d\n", treeHead, nodesAllocated );
            for ( int i = 0; i < nodeArray.size(); i++ )
            {
                printf( "node %d. left %d, right %d, color %#.2x%.2x%.2x\n", i, nodeArray[i].left, nodeArray[i].right,
                        nodeArray[i].B, nodeArray[i].G, nodeArray[i].R );
            }
        } //ShowTree

        #ifndef NDEBUG
        static bool UnitTest()
        {
            // add a somewhat random number of random colors to a tree

            srand( time( 0 ) );
            const DWORD items = 3000 + ( rand() % 1000 );
            KDTreeBGR kdtree( items );

            for ( DWORD i = 0; i < items; i++ )
            {
                int r = rand() % 256;
                int g = rand() % 256;
                int b = rand() % 256;

                // don't add duplicates

                bool found = false;
                for ( DWORD t = 1; t <= kdtree.NodeCount(); t++ )
                {
                    if ( 0 == kdtree.nodeArray[ t ].Distance( r, g, b ) )
                    {
                        found = true;
                        break;
                    }
                }

                if ( !found )
                    kdtree.Insert( r, g, b );
            }

            // Search for random colors using the tree and a linear search. Ensure the best distance for each is
            // the same. The color found may be different; that's OK, provided the distance is identical.

            for ( int t = 0; t < 5000; t++ )
            {
                int r = rand() % 256;
                int g = rand() % 256;
                int b = rand() % 256;

                DWORD idFound;
                int found = kdtree.Nearest( r, g, b, idFound );
                int distanceFound = kdtree.nodeArray[ idFound + 1 ].Distance( r, g, b );

                // do a linear search and compare the results. Slot 0 is empty.

                int idBest = 0;
                int distanceBest = INT_MAX;

                for ( ushort item = 1; item <= kdtree.NodeCount(); item++ )
                {
                    int distance = kdtree.nodeArray[ item ].Distance( r, g, b );

                    if ( distance < distanceBest )
                    {
                        idBest = item;
                        distanceBest = distance;
                    }
                }

                if ( distanceFound != distanceBest )
                {
                    printf( "kdtree unit test failure. items in the tree %d\n", kdtree.nodesAllocated );
                    printf( "linear search found a different best answer than the tree.\n" );
                    printf( "    test case %d, idFound %d, distanceFound %d, idBest %d, distanceBest %d\n",
                            t, idFound, distanceFound, idBest, distanceBest );
                    printf( "    looked for            r %#.2x, g %#.2x, b %#.2x\n", r, g, b );
                    KDNode & nodeFound = kdtree.nodeArray[ idFound + 1 ];
                    printf( "    tree found            r %#.2x, g %#.2x, b %#.2x\n", nodeFound.R, nodeFound.G, nodeFound.B );
                    KDNode & nodeBest = kdtree.nodeArray[ idBest ];
                    printf( "    linear search found   r %#.2x, g %#.2x, b %#.2x\n", nodeBest.R, nodeBest.G, nodeBest.B );
                    return false;
                }
            }

            return true;
        } //UnitTest
        #endif
}; //KDTreeBGR

