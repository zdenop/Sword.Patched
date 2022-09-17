#include <curlhttpt.h>
//#include <curlftpt.h>
//#include <ftplibftpt.h>
#include <filemgr.h>
#include <vector>
#include <iostream>

using sword::RemoteTransport;
using sword::CURLHTTPTransport;
using sword::DirEntry;
using std::vector;
using std::cout;
using std::endl;

int main(int argc, char **argv) {
	RemoteTransport *t = new CURLHTTPTransport("crosswire.org");
	vector<DirEntry> x = t->getDirList("https://crosswire.org/ftpmirror/pub/sword/raw/");
// ##
// ## These other transports should work the same as CURLHTTPTransport
// ## 
//	RemoteTransport *t = new sword::CURLFTPTransport("ftp.crosswire.org");
//	vector<DirEntry> x = t->getDirList("ftp://ftp.crosswire.org/pub/sword/raw/");
// ##
// ## FTPLib support is usually not compiled if CURL support is enabled
// ## To test this transport, re-configure sword (usrinst.sh: without-curl), recompile,
// ## reinstall, then copy this file over to examples/classes and make there.  The
// ## Makefile there will build any .cpp file against the installed libsword.
// ## 
//	RemoteTransport *t = new sword::FTPLibFTPTransport("ftp.crosswire.org");
//	vector<DirEntry> x = t->getDirList("ftp://ftp.crosswire.org/pub/sword/raw/");
	for (vector<DirEntry>::const_iterator i = x.begin(); i != x.end(); ++i) {
		cout << i->name << "\t" << i->size << "\t" << i->isDirectory << endl;
	}
	return 0;
}
