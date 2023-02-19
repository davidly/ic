#pragma once

template <class T> void Swap( T & a, T & b )
{
    T c = a;
    a = b;
    b = c;
} //Swap

unsigned long ulrand()
{
    unsigned long r = 0;

    for ( int i = 0; i < 5; i++ )
        r = ( r << 15 ) | ( rand() & 0x7FFF );

    return r;
} //ulrand

template <typename T> void Randomize( vector<T> & v )
{
    size_t s = v.size();

    for ( size_t i = 0; i < s; i++ )
    {
        size_t r = ulrand() % s;
        Swap( v[ i ], v[ r ] );
    }
} //Randomize


