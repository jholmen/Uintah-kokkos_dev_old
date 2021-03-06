/*
 * The MIT License
 *
 * Copyright (c) 1997-2016 The University of Utah
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef UINTAH_CORE_UTIL_DOUT_HPP
#define UINTAH_CORE_UTIL_DOUT_HPP

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <string>
#include <sstream>

#include <sci_defs/mpi_defs.h>


#define DOUT( cond, ... )             \
  if (cond) {                         \
    std::ostringstream msg;           \
    msg << __VA_ARGS__;               \
    printf("%s\n",msg.str().c_str()); \
  }

#define POUT( ... )                   \
  {                                   \
    std::ostringstream msg;           \
    msg << __FILE__ << ":";           \
    msg << __LINE__ << " : ";         \
    msg << __VA_ARGS__;               \
    printf("%s\n",msg.str().c_str()); \
  }

#define TOUT()                            \
  printf("TOUT:  %d  %d  %s:%d\n"         \
      , Uintah::MPI::Impl::prank( MPI_COMM_WORLD )\
      , Uintah::MPI::Impl::tid()                  \
      , __FILE__                          \
      , __LINE__                          \
      )


#define DOUTP0( cond, ... )                                 \
  if ( Uintah::MPI::Impl::prank( MPI_COMM_WORLD ) == 0 && cond) {   \
    std::ostringstream msg;                                 \
    msg << __FILE__ << ":";                                 \
    msg << __LINE__ << " : ";                               \
    msg << __VA_ARGS__;                                     \
    printf("%s\n",msg.str().c_str());                       \
  }


namespace Uintah {

class Dout
{

public:

  Dout()               = default;
  Dout( const Dout & ) = default;
  Dout( Dout && )      = default;

  Dout & operator=( const Dout & ) = default;
  Dout & operator=( Dout && )      = default;

  Dout( std::string const & name, bool default_active )
    : m_active{ is_active(name, default_active) }
    , m_name{ name + (m_active ? ":+" : ":-") }
  {}

  explicit operator bool() const { return m_active; }

  const std::string & name () const { return m_name ; }

  friend bool operator<(const Dout & a, const Dout &b )
  {
    return a.m_name < b.m_name;
  }


private:

  static bool is_active( std::string const& arg_name,  bool default_active )
  {
    const char * sci_debug = std::getenv("SCI_DEBUG");
    const std::string tmp = "," + arg_name + ":";
    const char * name = tmp.c_str();
    size_t n = tmp.size();

    if ( !sci_debug ) {
      return default_active;
    }

    const char * sub =  strstr(sci_debug, name);
    if ( !sub ) {
      sub = strstr( sci_debug, name+1);
      --n;
      // name not found
      if ( !sub || sub != sci_debug ) {
        return default_active;
      }
    }
    return sub[n] == '+';
  }


private:

  bool        m_active {false};
  std::string m_name   {""};
};

} //namespace Uintah

#endif //UINTAH_CORE_UTIL_DOUT_HPP
