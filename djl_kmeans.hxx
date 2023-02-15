#pragma once

// copied originally from https://github.com/aditya1601/kmeans-clustering-cpp
// changes mostly to:
//    add bgr-specific handling
//    add asserts
//    find better initial cluster centroids, with more difference between them
//    optionally return the nearest points to the centroids, rather than synthetic centroids
//    performance:
//        overall perf is >15x faster than original
//        reduce copy constructor usage for complex objects
//        use PPL for parallelization
//        don't use sqrt() since it doesn't add value
//        remove pre-loop-setup-iterations
//        don't allocate vectors for point values
//        better use the CPU cache by accessing memory in order
//    to do:
//        Add new run() method that takes a range of K, resulting in whichever K has the lowest standard deviations

#include <ppl.h>
#include <vector>
#include <limits>

using namespace std;

// really doesn't matter except that it's slower
//#define KMEANS_USE_SQRT

class KMeansPoint
{
    private:
        int pointId, clusterId;
        static const int ValueCount = 3;
        double values[ ValueCount ];
    
    public:
        KMeansPoint( int id, int r, int g, int b ) :
            pointId( id ),
            clusterId( 0 )  // not assigned to any cluster
        {
            values[ 0 ] = (double) r / 255.0;
            values[ 1 ] = (double) g / 255.0;
            values[ 2 ] = (double) b / 255.0;
        }

        DWORD getBGR()
        {
            int r = round( getVal( 0 ) * 255.0 );
            int g = round( getVal( 1 ) * 255.0 );
            int b = round( getVal( 2 ) * 255.0 );
    
            return b | ( g << 8 ) | ( r << 16 );
        } //getBGR()
    
        int dimensionCount() { return ValueCount; }
        int getCluster() { return clusterId; }
        int getID() { return pointId; }
        void setCluster( int val ) { clusterId = val; }
        double getVal( int pos ) { return values[ pos ]; }
        void setVal( int pos, double val ) { values[ pos ] = val; }

        double distance( KMeansPoint & other )
        {
            double sum = 0.0;
            for ( int j = 0; j < ValueCount; j++ )
            {
                double val = values[ j ] - other.getVal( j );
                sum += ( val * val );
            }
    
            #ifdef KMEANS_USE_SQRT
                double dist = sqrt( sum );
            #else
                double dist = sum;
            #endif

            assert( dist >= 0.0 );
            return dist;
        } //distance
};

class KMeansCluster
{
    private:
        int clusterId;                   // 1-based ID.
        KMeansPoint centroid;            // not necessarily the same as any point in the cluster
        vector<double> scoreScratchpad;  // temporary area to avoid allocations when multi-threaded

        // these pointers aren't owned by the vector; they're just pointers to items passed
        // to KMeans::run, so the lifetime of the values is up to the caller.

        vector<KMeansPoint *>  points;     
    public:
        KMeansCluster( int id, KMeansPoint & cent ) :
            clusterId( id ),
            centroid( cent ),  // would like to remove this copy constructor usage
            scoreScratchpad( cent.dimensionCount() ) // handy place to sum scores
        {
            assert( 0 != id );
            addPoint( cent );
        }

        static int compareClusters( const void * a, const void * b )
        {
            // sort by size of cluster (# of items contained) high to low

            KMeansCluster const * pa = (KMeansCluster const *) a;
            KMeansCluster const * pb = (KMeansCluster const *) b;

            return pb->points.size() - pa->points.size();
        } //compareClusters
    
        void addPoint( KMeansPoint & p )
        {
            p.setCluster( clusterId );

            points.push_back( & p );
        } //addPoint

        void zeroScratchpad()
        {
            for ( int i = 0; i < scoreScratchpad.size(); i++ )
                scoreScratchpad[ i ] = 0.0;
        } //zeroScratchpad

        void sumScratchpad( int pos, double val ) { scoreScratchpad[ pos ] += val; }
        double getScratchpad( int pos ) { return scoreScratchpad[ pos ]; }
        void removeAllPoints() { points.clear(); }
        int getId() { return clusterId; }
        KMeansPoint & getPoint( int pos ) { return * points[ pos ]; }
        KMeansPoint & getCentroid() { return centroid; }
        int getSize() { return points.size(); }
        double getCentroidByPos( int pos ) { return centroid.getVal( pos ); }
        void setCentroidByPos( int pos, double val ) { centroid.setVal( pos, val ); }
};

class KMeans
{
    private:
        int K, iters, dimensions, total_points;
        vector<KMeansCluster> clusters;
    
        void clearClusters()
        {
            for ( int i = 0; i < K; i++ )
                clusters[ i ].removeAllPoints();
        } //clearClusters
    
        int getNearestClusterId( KMeansPoint & point )
        {
            double min_dist = DBL_MAX;
            int nearestClusterId;

            for ( int i = 0; i < K; i++ )
            {
                KMeansCluster & cluster = clusters[ i ];

                double dist = point.distance( cluster.getCentroid() );
                assert( dist >= 0.0 );

                if ( dist < min_dist )
                {
                    min_dist = dist;
                    nearestClusterId = cluster.getId();
                }
            }
    
            return nearestClusterId;
        } //getNearestClusterId
    
    public:
        KMeans( int K, int iterations )
        {
            assert( K > 0 ); // caller error
            this->K = K;
            this->iters = iterations;
        } //KMeans
    
        void run( vector<KMeansPoint> & all_points, int seed_iterations = 40 )
        {
            clusters.clear();
            total_points = all_points.size();

            if ( total_points < K )
            {
                assert( total_points >= K ); // caller error
                return;
            }

            if ( seed_iterations <= 0 )
                seed_iterations = 1;

            dimensions = all_points[ 0 ].dimensionCount();

            if ( total_points == K )
            {
                for ( int i = 0; i < K; i++ )
                {
                    all_points[ i ].setCluster( i + 1 );
                    clusters.emplace_back( i + 1, all_points[ i ]  );
                }
            }
            else
            {
                vector<int> best_pointIds( K );
                double best_distance = 0.0;
                int best_start = 0;
                const int best_iterations = seed_iterations;

                // try n times to find initial centroids that are as different from each other as possible

                for ( int r = 0; r < best_iterations; r++ )
                {
                    //printf( "pass %d\n", r );
                    vector<int> used_pointIds;

                    for ( int i = 1; i <= K; i++ )
                    {
                        // this may be an infinite loop if rand() is badly behaved
    
                        while ( true )
                        {
                            int index = rand() % total_points;
            
                            if ( find( used_pointIds.begin(), used_pointIds.end(), index ) == used_pointIds.end() )
                            {
                                used_pointIds.push_back( index );
                                break;
                            }
                        }
                    }

                    // see how far away each initial centroid is from every other initial centroid

                    double distance = 0;
                    for ( int i = 0; i < K; i++ )
                    {
                        for ( int j = i + 1; j < K; j++ )
                        {
                            double d = all_points[ used_pointIds[ i ] ].distance( all_points[ used_pointIds[ j ] ] );
                            distance += d;
                            //printf( "  so far: %lf total %lf\n", d, distance );
                        }
                    }

                    // choose the one with the most total distance

                    //printf( "  distances for pass %d: %lf\n", r, distance );

                    if ( distance > best_distance )
                    {
                        //printf( "  new best starting centroids %d, distance %lf\n", r, distance );
                        best_distance = distance;
                        best_start = r;
                        best_pointIds.clear();
                        best_pointIds = used_pointIds;
                    }
                }

                //printf( "best starting centroid attempt: %d of %d\n", best_start, best_iterations );

                // setup clustering with the best set of initial centroids

                for ( int i = 1; i <= K; i++ )
                {
                    int index = best_pointIds[ i - 1 ];
                    all_points[ index ].setCluster( i );
                    clusters.emplace_back( i, all_points[ index ]  );
                }
            }

            int iter = 1;
            do
            {
                bool done = true;
    
                // Add all points to their nearest cluster

                //for ( int i = 0; i < total_points; i++ )
                parallel_for ( 0, total_points, [&] ( int i )
                {
                    int currentClusterId = all_points[ i ].getCluster();
                    int nearestClusterId = getNearestClusterId( all_points[ i ] );
    
                    if ( currentClusterId != nearestClusterId )
                    {
                        all_points[ i ].setCluster( nearestClusterId ) ;
                        done = false;
                    }
                } );
    
                clearClusters();

                // do this in the loop below instead. That's slower but more parallelized.
                //for ( int i = 0; i < total_points; i++ )
                //    clusters[ all_points[ i ].getCluster() - 1 ].addPoint( all_points[ i ] );

                // Recalculate the synthetic center of each cluster

                //for ( int i = 0; i < K; i++ )
                parallel_for( 0, K, [&] ( int i )
                {
                    KMeansCluster & cluster = clusters[ i ];

                    for ( int i = 0; i < total_points; i++ )
                        if ( cluster.getId() == all_points[ i ].getCluster() )
                            cluster.addPoint( all_points[ i ] );

                    int clusterSize = cluster.getSize();

                    if ( clusterSize > 0 )
                    {
                        cluster.zeroScratchpad();

                        for ( int p = 0; p < clusterSize; p++ )
                        {
                            KMeansPoint & point = cluster.getPoint( p );

                            for ( int j = 0; j < dimensions; j++ )
                                cluster.sumScratchpad( j, point.getVal( j ) );
                        }

                        double dClusterSize = (double) clusterSize;

                        for ( int j = 0; j < dimensions; j++ )
                            cluster.setCentroidByPos( j, cluster.getScratchpad( j ) / dClusterSize );
                    }
                } );
    
                if ( done || iter >= iters )
                    break;

                iter++;
            } while ( true );

            //printf( "ran %d iterations\n", iter );
        } //run

        void sort()
        {
            qsort( clusters.data(), K, sizeof KMeansCluster, KMeansCluster::compareClusters );
        }
    
        void showbgr()
        {
            printf( "static DWORD colors[] = \n{\n" );
            for ( int i = 0; i < K; i++ )
            {
                KMeansCluster & cluster = clusters[ i ];
                printf( "    %#x, // count %d\n", cluster.getCentroid().getBGR(), clusters[i].getSize() );
            }
            printf( "};\n" );
        } //showbgr

        void getbgrSynthetic( vector<DWORD> & centroids )
        {
            // return the synthetic centoid values, which may not be found in the input dataset

            centroids.resize( K );

            for ( int i = 0; i < K; i++ )
            {
                KMeansCluster & cluster = clusters[ i ];
                centroids[ i ] = cluster.getCentroid().getBGR();
            }
        } //getbgrSynthetic

        double getbgrClosest( vector<DWORD> & closest, vector<int> & pointIDs )
        {
            // Return the actual values from the input dataset that are closest to the synthetic centroids
            // Return value is the average standard deviation for each cluster.

            closest.resize( K );
            pointIDs.resize( K );
            vector<double> distances( K );

            //for ( int i = 0; i < K; i++ )
            parallel_for( 0, K, [&] ( int i )
            {
                KMeansCluster & cluster = clusters[ i ];
                KMeansPoint & centroid = cluster.getCentroid();
                double min_dist = DBL_MAX;
                int best = 0;

                for ( int p = 0; p < cluster.getSize(); p++ )
                {
                    double dist = centroid.distance( cluster.getPoint( p ) );
                    if ( dist < min_dist )
                    {
                        min_dist = dist;
                        best = p;
                    }
                    distances[ i ] += dist;
                }

                KMeansPoint & point = cluster.getPoint( best );
                closest[ i ] = point.getBGR();
                pointIDs[ i ] = point.getID();
                distances[ i ] /= cluster.getSize();
                //printf( "cluster size: %d\n", cluster.getSize() );
             } );

             double sum = 0.0;
             for ( int i = 0; i < K; i++ )
                 sum += distances[ i ];
             double avg = sum / K;

             //printf( "average standard deviation: %lf\n", avg );

             return avg;
        } //getbgrClosest

        void getbgrClosest( vector<DWORD> & closest )
        {
            // Return the actual value that's closest to the centroid, not the synthetic
            // centroid value that may not map to a real input value.

            closest.resize( K );

            //for ( int i = 0; i < K; i++ )
            parallel_for( 0, K, [&] ( int i )
            {
                KMeansCluster & cluster = clusters[ i ];
                KMeansPoint & centroid = cluster.getCentroid();
                double min_dist = DBL_MAX;
                int best = 0;

                for ( int i = 0; i < cluster.getSize(); i++ )
                {
                    double dist = centroid.distance( cluster.getPoint( i ) );
                    if ( dist < min_dist )
                    {
                        min_dist = dist;
                        best = i;
                    }
                }

                closest[ i ] = cluster.getPoint( best ).getBGR();
             } );

        } //getbgrClosest

        void getClusterbgrItems( int c, vector<DWORD> & items )
        {
            assert( c < K );
            KMeansCluster & cluster = clusters[ c ];
            items.resize( cluster.getSize() );

            for ( int i = 0; i < cluster.getSize(); i++ )
                items[ i ] = cluster.getPoint( i ).getBGR();
        } //getCluserbgrItems
};

