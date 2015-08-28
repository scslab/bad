#include "tune_knobs.hh"

#include "util.hh"

#include "meth1_memory.hh"
#include "meth1_merge.hh"
#include "node.hh"

using namespace std;
using namespace meth1;

/* Construct Node */
Node::Node( vector<string> files, string port, bool odirect )
  : tg_{}
  , recios_{}
  , port_{port}
  , last_{Rec::MIN}
  , fpos_{0}
  , seek_chunk_{calc_record_space()}
  , lpass_{0}
  , size_{0}
{
  if ( files.size() <= 0 ) {
    throw runtime_error( "No files to read from" );
  }

  cout << "seek-chunk, " << seek_chunk_ << endl;
  for ( auto & f : files ) {
    recios_.emplace_back( f, odirect ? O_RDONLY | O_DIRECT : O_RDONLY );
  }
}

void Node::Initialize( void ) { return; }

/* Run the node - list and respond to RPCs */
void Node::Run( void )
{
  TCPSocket sock{IPV4};
  sock.set_reuseaddr();
  sock.bind( {"0.0.0.0", port_} );
  sock.listen();

  while ( true ) {
    try {
      BufferedIO_O<TCPSocket> client {sock.accept()};
      while ( true ) {
        const char * str = client.read_buf_all( 1 ).first;
        if ( client.eof() ) {
          break;
        }
        switch ( str[0] ) {
        case 0:
          RPC_Read( client );
          break;
        case 1:
          RPC_Size( client );
          break;
        default:
          throw runtime_error( "Unknown RPC method: " + to_string(str[0]) );
          break;
        }
      }
    } catch ( const exception & e ) {
      // EOF
    }
  }
}

void Node::RPC_Read( BufferedIO_O<TCPSocket> & client )
{
  const char * str = client.read_buf_all( 2 * sizeof( uint64_t ) ).first;
  uint64_t pos = *( reinterpret_cast<const uint64_t *>( str ) );
  uint64_t amt = *( reinterpret_cast<const uint64_t *>( str ) + 1 );

  RecV recs;
  if ( pos < Size() ) {
    recs = Read( pos, amt );
  }

  uint64_t siz = recs.size();
  client.write_all( reinterpret_cast<const char *>( &siz ), sizeof( uint64_t ) );
  for ( auto const & r : recs ) {
    r.write( client );
  }
  client.flush( true );
}

void Node::RPC_Size( BufferedIO_O<TCPSocket> & client )
{
  uint64_t siz = Size();
  client.write_all( reinterpret_cast<const char *>( &siz ),
                    sizeof( uint64_t ) );
  client.flush( true );
}

Node::RecV Node::Read( uint64_t pos, uint64_t size )
{
  static size_t pass = 0;

  if ( pos + size > Size() ) {
    size = Size() - pos;    
  }

  auto t0 = time_now();
  auto recs = linear_scan( seek( pos ), size );
  if ( recs.size() > 0 ) {
    last_.copy( recs.back() );
    fpos_ = pos + recs.size();
  }
  cout << "do-read, " << ++pass << ", " << time_diff<ms>( t0 ) << endl << endl;

  return recs;
}

uint64_t Node::Size( void )
{
  if ( size_ == 0 ) {
    size_ = accumulate( recios_.begin(), recios_.end(), 0,
      []( size_t res, RecLoader & r ) { return res + r.records(); } );
  }
  return size_;
}

/* Return the record that corresponds to the specified position. */
Record Node::seek( uint64_t pos )
{
  // can we continue from last time?
  if ( pos == 0 ) {
    last_ = Rec::MIN;
  } else if ( pos >= Size() ) {
    last_ = Rec::MAX;
  } else if ( fpos_ != pos ) {
    // remember, retrieving the record just before `pos`
    for ( uint64_t i = 0; i < pos; i += seek_chunk_ ) {
      auto recs = linear_scan( Record{Rec::MIN}, min( pos - i, seek_chunk_ ) );
      if ( recs.size() == 0 ) {
        break;
      }
      last_.copy( recs.back() );
    }
  }
  return last_;
}

/* Perform a single linear scan of the file, returning the next `size` smallest
 * records that occur directly after the `after` record. */
Node::RecV Node::linear_scan( const Record & after, uint64_t size )
{
  cout << "linear-scan, " << ++lpass_ << ", " << timestamp<ms>()
    << ", start" << endl;
  if ( size == 1 ) {
    return linear_scan_one( after );
  } else {
    return linear_scan_chunk( after, size );
  }
}

/* Linear scan optimized for retrieving just one record. */
Node::RecV Node::linear_scan_one( const Record & after )
{
  auto t0 = time_now();

  // scan all files
  vector<RR> rr( recios_.size() );
  for ( size_t i = 0; i < recios_.size(); i++ ) {
    RecLoader & rio = recios_[i];
    RR * rr_i = &rr[i];

    rio.rewind();
    tg_.run( [&rio, &after, rr_i]() {

      RR min( Rec::MAX );
      while ( true ) {
        RecordPtr next = rio.next_record();
        if ( rio.eof() ) {
          break;
        }
        if ( next > after and min > next ) {
          min.copy( next );
        }
      }
      rr_i->copy( min );
    } );
  }
  tg_.wait();

  // find min of all files
  RR * min = &rr[0];
  for ( size_t i = 1; i < recios_.size(); i++ ) {
    if ( rr[i] < *min ) {
      min = &rr[i];
    }
  }
  vector<RR> rec = { move( *min ) };
  auto tt = time_diff<ms>( t0 );
  cout << "linear-scan, " << lpass_ << ", " << tt << endl;

  return {move( rec )};
}

/* Linear scan using a chunked sorting + merge strategy. */
Node::RecV Node::linear_scan_chunk( const Record & after, uint64_t size,
    RR * r1, RR * r2, RR *r3, uint64_t r1x )
{
  auto t0 = time_now();
  tdiff_t tm = 0, ts = 0, tl = 0;

  const RR * curMin = nullptr;
  const uint64_t r1x_i = r1x / recios_.size();
  vector<uint64_t> r1s_i( recios_.size() );
  uint64_t r2s = 0;

  // kick of all readers
  for ( auto & rio : recios_ ) {
    rio.rewind();
  }

  while ( true ) {
    // FILTER - DiskIO
    uint64_t rio_i = 0;
    for ( auto & rio : recios_ ) {
      if ( not rio.eof() ) {
        tg_.run( [&rio, rio_i, &r1s_i, r1, r1x_i, &after, curMin]() {
          r1s_i[rio_i] =
            rio.filter( &r1[r1x_i * rio_i], r1x_i, after, curMin );
        } );
        rio_i++;
      }
    }
    tg_.wait();

    // EOF?
    if ( rio_i == 0 ) {
      break;
    }

    // CREATE CONTIGUOUS SORT BUFFER
    uint64_t r1s = r1s_i[0];
    bool moving = false;
    for ( uint64_t i = 0; i < rio_i - 1; i++ ) {
      moving |= r1s_i[i] < r1x_i;
      if ( moving ) {
        uint64_t i1_start = r1x_i * (i + 1);
        uint64_t i1_end = i1_start + r1s_i[i + 1];
        move( &r1[i1_start], &r1[i1_end], &r1[r1s] );
      }
      r1s += r1s_i[i+1];
    }

    // SORT + MERGE
    if ( r1s > 0 ) {
      // SORT
      auto ts1 = time_now();
      rec_sort( r1, r1 + r1s );
      ts += time_diff<ms>( ts1 );

      // MERGE
      if ( Knobs::PARALLEL_MERGE and Knobs::USE_COPY ) {
        tm += meth1_pmerge_copy( r1, r1+r1s, r2, r2+r2s, r3, r3+size );
      } else if ( Knobs::PARALLEL_MERGE ) {
        tm += meth1_pmerge_move( r1, r1+r1s, r2, r2+r2s, r3, r3+size );
      } else if( Knobs::USE_COPY ) {
        tm += meth1_merge_copy( r1, r1 + r1s, r2, r2 + r2s, r3, r3 + size );
      } else {
        tm += meth1_merge_move( r1, r1 + r1s, r2, r2 + r2s, r3, r3 + size );
      }

      // PREP
      swap( r2, r3 );
      r2s = min( size, r1s + r2s );
      if ( r2s == size ) {
        curMin = &r2[size - 1];
      }
      tl = time_diff<ms>( ts1 );
    }
  }
  auto t1 = time_now();

  cout << "total, " << time_diff<ms>( t1, t0 ) << endl;
  cout << "sort , " << ts << endl;
  cout << "merge, " << tm << endl;
  cout << "last , " << tl << endl;
  
  return {r2, r2s};
}

void free_buffers( Node::RR * r1, Node::RR * r3, size_t size )
{
  if ( Knobs::USE_COPY ) {
    // r3 and r2 share storage cells, so clear to nullptr first.
    for ( uint64_t i = 0; i < size; i++ ) {
      r3[i].set_val( nullptr );
    }
  }
  delete[] r3;
  delete[] r1;
}

/* Memory management for linear_scan_chunk */
Node::RecV Node::linear_scan_chunk( const Record & after, uint64_t size )
{
  if ( size == 0 ) {
    return {nullptr, 0};
  }

  // our globals
  static size_t gr1x = 0, gr2x = 0;
  static RR *gr1 = nullptr, *gr2 = nullptr, *gr3 = nullptr;

  // local variables
  RR *r1, *r2, *r3;
  size_t r1x;

  if ( Knobs::REUSE_MEM ) {
    // (re-)setup global buffers if size isn't correct
    if ( gr2x != size ) {
      if ( gr2x > 0 ) {
        free_buffers( gr1, gr3, gr2x );
        delete[] gr2;
      }
      // r1x = max( (uint64_t) 3145728, (uint64_t) 1 );
      gr1x = max( size / Knobs::SORT_MERGE_RATIO, (uint64_t) 1 );
      gr2x = size;
      gr1 = new RR[gr1x];
      gr2 = new RR[gr2x];
      gr3 = new RR[gr2x];
    } else {
      for ( uint64_t i = 0; i < size; i++ ) {
        gr3[i].set_val( nullptr );
      }
    }
    r1 = gr1; r2 = gr2; r3 = gr3; r1x = gr1x;
  } else { /* !REUSE_MEM */
    // r1x = max( (uint64_t) 3145728, (uint64_t) 1 );
    r1x = max( size / Knobs::SORT_MERGE_RATIO, (uint64_t) 1 );
    r1 = new RR[r1x];
    r2 = new RR[size];
    r3 = new RR[size];
  }

  cout << "linear-scan-chunk, " << size << ", " << r1x << endl;
  auto rr = linear_scan_chunk( after, size, r1, r2, r3, r1x );
  if ( rr.data() == r3 ) {
    swap( r2, r3 );
    if ( Knobs::REUSE_MEM ) {
      swap( gr2, gr3 );
    }
  }
  
  if ( not Knobs::REUSE_MEM ) {
    free_buffers( r1, r3, size );
  } else {
    /* we don't want r2 being freed later! */
    rr.own() = false;
  }

  return rr;
}
