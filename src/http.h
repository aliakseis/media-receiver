/*
 * This file is part of the sse package, copyright (c) 2011, 2012, @radiospiel.
 * It is copyrighted under the terms of the modified BSD license, see LICENSE.BSD.
 *
 * For more information see https://https://github.com/radiospiel/sse.
 */

#ifndef HTTP_H
#define HTTP_H

#include <string.h>
#include <curl/curl.h>

#include <functional>

/*
 * send HTTP request.
 */

enum HttpVerb {
    HTTP_GET = 0,
    HTTP_POST = 1,
};

typedef std::function<size_t(char*, size_t, size_t)> OnDataFunc;

typedef std::function<size_t(curl_off_t, curl_off_t, curl_off_t, curl_off_t)> OnProgressFunc;

//extern 
bool http(HttpVerb  verb,
  const char*   url, 
  const char**  http_headers, 

  const char*   body, 
  unsigned      bodyLenght,

  OnDataFunc    on_data = {},
  std::function<const char*(CURL*)> on_verify = {},
  OnProgressFunc progress_callback = {}

/*
  size_t        (*on_data)(char *ptr, size_t size, size_t nmemb, void *userdata) = nullptr,
  const char*   (*on_verify)(CURL* curl) = nullptr,
  size_t        (*progress_callback)(void *clientp, // https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html
        curl_off_t dltotal,
        curl_off_t dlnow,
        curl_off_t ultotal,
        curl_off_t ulnow) = nullptr
        */
);

//extern char curl_error_buf[];

//extern 
//size_t http_ignore_data(char *ptr, size_t size, size_t nmemb, void *userdata);


//#define DECLARE_OBJECT(T, name) extern struct T name
//#define DEFINE_OBJECT(T, name)  struct T name = T ## _Initializer

/*
 * Aplication options
 */
struct Options {
    const char *arg0;           // process name
    //const char *url;            // URL to get
    int         limit;          // event limit
    int         verbosity;      // verbosity
    int         allow_insecure; // allow insecure connections
    const char *ssl_cert;       // SSL cert file
    const char *ca_info;        // CA cert file
    //char       **command;       // command to run (if any)
};

//#define Options_Initializer {0,0,0,0,0,0,0,0}
//DECLARE_OBJECT(Options, options);

extern Options options;

#endif
