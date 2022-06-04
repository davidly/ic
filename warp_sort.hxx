#pragma once

// taken from http://warp.povusers.org/SortComparison/index.html
// ~20% faster than vector::sort for some use cases

#include <algorithm>

template<typename T>
void InsertionSort( T * array, unsigned size )
{
    for ( unsigned i = 1; i < size; ++i )
    {
        T val = array[i];
        unsigned j = i;
        while ( j > 0 && val < array[j-1] )
        {
            array[j] = array[j-1];
            --j;
        }
        array[j] = val;
    }
}

template<typename T>
unsigned Partition( T * array, unsigned f, unsigned l, T pivot )
{
    unsigned i = f-1, j = l+1;

    do
    {
        while ( pivot < array[--j] );
        while ( array[++i] < pivot );

        if ( i < j )
        {
            T tmp = array[i];
            array[i] = array[j];
            array[j] = tmp;
        }
        else
            return j;
    } while ( true );
}

template<typename T>
void QuickSortImpl( T * array, unsigned f, unsigned l )
{
    while( f < l )
    {
        unsigned m = Partition( array, f, l, array[f] );
        QuickSortImpl( array, f, m );
        f = m+1;
    }
}

template<typename T>
void QuickSort( T * array, unsigned size )
{
    QuickSortImpl( array, 0, size-1 );
}

template<typename T>
void MedianHybridQuickSortImpl( T * array, unsigned f, unsigned l )
{
    while ( f+16 < l )
    {
        T v1 = array[f], v2 = array[l], v3 = array[(f+l)/2];
        T median = v1 < v2 ? ( v3 < v1 ? v1 : __min( v2, v3 ) ) :
                             ( v3 < v2 ? v2 : __min( v1, v3 ) );
        unsigned m = Partition( array, f, l, median );
        MedianHybridQuickSortImpl( array, f, m );
        f = m+1;
    }
}

template<typename T>
void MedianHybridQuickSort( T * array, unsigned size )
{
    MedianHybridQuickSortImpl( array, 0, size-1 );
    InsertionSort( array, size );
}

