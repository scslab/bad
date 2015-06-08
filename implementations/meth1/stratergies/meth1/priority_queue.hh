#ifndef PRIORITY_QUEUE_HH
#define PRIORITY_QUEUE_HH

#include <queue>

#include "exception.hh"

namespace mystl
{

/**
 * Custom priority queue (only vector backed) that exposes the vector reserve
 * operations and the underlying vector for performance improvements.
 */
template <class T> class priority_queue : public std::priority_queue<T>
{
public:
  using size_type = typename std::priority_queue<T>::size_type;

  priority_queue( size_type capacity = 0 )
  {
    if ( capacity > 0 ) {
      reserve( capacity );
    }
  }

  void reserve( size_type capacity ) { this->c.reserve( capacity ); }

  size_type capacity( void ) const { return this->c.capacity(); }

  std::vector<T> container( void ) { return this->c; }
};
}

#endif /* PRIORITY_QUEUE_HH */