#include <fcntl.h>

#include <cassert>

#include "file.hh"
#include "exception.hh"

using namespace std;

/* construct from fd number */
File::File( const int fd )
  : FileDescriptor( fd )
{
}

/* construct by opening file at path given */
File::File( const char * path, int flags )
  : File( SystemCall( "open", open( path, flags ) ) )
{
}

/* construct by opening file at path given */
File::File( const char * path, int flags, mode_t mode )
  : File( SystemCall( "open", open( path, flags, mode ) ) )
{
}

/* move constructor */
File::File( File && other )
  : FileDescriptor( std::move( other ) )
{
}

/* destructor */
File::~File() {}