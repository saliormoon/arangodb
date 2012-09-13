////////////////////////////////////////////////////////////////////////////////
/// @brief url class
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2011 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Achim Brandt
/// @author Copyright 2007-2011, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Url.h"

#include <regex.h>

#include <iostream>

#include <Basics/Exceptions.h>
#include <Basics/StringUtils.h>

using namespace std;
using namespace triagens::basics;
using namespace triagens::rest;

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Url
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief URL pattern
////////////////////////////////////////////////////////////////////////////////

static regex_t re;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Url
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

Url::Url (string const& urlName)
  : _host("localhost"),
    _port(80),
    _service("http"),
    _path("") {
  int result;
  regmatch_t matches[5];

  result = regexec(&re, urlName.c_str(), sizeof(matches) / sizeof(matches[0]), matches, 0);

  if (result == 0) {
    char const* ptr = urlName.c_str();
    string port = string(ptr + matches[3].rm_so, matches[3].rm_eo - matches[3].rm_so);

    _host = string(ptr + matches[2].rm_so, matches[2].rm_eo - matches[2].rm_so);
    _port = port.empty() ? 80 : StringUtils::uint32(port.substr(1));
    _path = string(ptr + matches[4].rm_so, matches[4].rm_eo - matches[4].rm_so);

    if (matches[1].rm_so == matches[1].rm_eo) {
      _service = "http";
    }
    else {
      _service = string(ptr + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so - 1);
    }
  }
  else {
    THROW_PARAMETER_ERROR("url", "url not valid", "constructor");
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Url
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the service part
////////////////////////////////////////////////////////////////////////////////

string const& Url::service () const {
  return _service;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the host part
////////////////////////////////////////////////////////////////////////////////

string const& Url::host () const {
  return _host;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the port
////////////////////////////////////////////////////////////////////////////////

uint16_t Url::port () const {
  return _port;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the path part
////////////////////////////////////////////////////////////////////////////////

string const& Url::path () const {
  return _path;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                            MODULE
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Url
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief already initialised
////////////////////////////////////////////////////////////////////////////////

static bool Initialised = false;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Url
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises the hashes components
////////////////////////////////////////////////////////////////////////////////

void TRI_InitialiseUrl () {
  if (Initialised) {
    return;
  }

  regcomp(&re, "([a-z]*:)//([a-z0-9\\._-]*)(:[0-9]+)?(/.*)", REG_ICASE | REG_EXTENDED);

  Initialised = true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief shut downs the hashes components
////////////////////////////////////////////////////////////////////////////////

void TRI_ShutdownUrl () {
  regfree(&re);
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:
