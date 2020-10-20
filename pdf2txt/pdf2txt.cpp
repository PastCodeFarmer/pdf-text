// pdf2txt.cpp : Defines the entry point for the console application.
//

//========================================================================
//
// pdftotext.cc
//
// Copyright 1997-2013 Glyph & Cog, LLC
//
//========================================================================

#include <aconf.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifdef DEBUG_FP_LINUX
#  include <fenv.h>
#  include <fpu_control.h>
#endif
#include "gmem.h"
#include "gmempp.h"
#include "parseargs.h"
#include "GString.h"
#include "GlobalParams.h"
#include "Object.h"
#include "Stream.h"
#include "Array.h"
#include "Dict.h"
#include "XRef.h"
#include "Catalog.h"
#include "Page.h"
#include "PDFDoc.h"
#include "TextOutputDev.h"
#include "CharTypes.h"
#include "UnicodeMap.h"
#include "TextString.h"
#include "Error.h"
#include "config.h"

static int firstPage = 1;
static int lastPage = 0;
static GBool physLayout = gFalse;
static GBool simpleLayout = gFalse;
static GBool tableLayout = gFalse;
static GBool linePrinter = gFalse;
static GBool rawOrder = gFalse;
static double fixedPitch = 0;
static double fixedLineSpacing = 0;
static GBool clipText = gFalse;
static GBool discardDiag = gFalse;
static char textEncName[128] = "";
static char textEOL[16] = "";
static GBool noPageBreaks = gFalse;
static GBool insertBOM = gFalse;
static char ownerPassword[33] = "\001";
static char userPassword[33] = "\001";
static GBool quiet = gFalse;
static char cfgFileName[256] = "";
static GBool printVersion = gFalse;
static GBool printHelp = gFalse;

static char encoding_table[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
                                'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                                'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                                'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                                'w', 'x', 'y', 'z', '0', '1', '2', '3',
                                '4', '5', '6', '7', '8', '9', '+', '/'};
static char *decoding_table = NULL;
static int mod_table[] = {0, 2, 1};


void build_decoding_table() {

    decoding_table = (char*)malloc(256);

    for (int i = 0; i < 64; i++)
        decoding_table[(unsigned char) encoding_table[i]] = i;
}


unsigned char *base64_decode( char *data,
                             size_t input_length,
                             size_t *output_length) {

    if (decoding_table == NULL) 
        build_decoding_table();

    if (input_length % 4 != 0) return NULL;

    *output_length = input_length / 4 * 3;
    if (data[input_length - 1] == '=') (*output_length)--;
    if (data[input_length - 2] == '=') (*output_length)--;

    unsigned char *decoded_data = (unsigned char *) malloc((*output_length)+3);
    //unsigned char *decoded_data = new (unsigned char) [(*output_length)];
    if (decoded_data == NULL) return NULL;

    for (int i = 0, j = 0; i < input_length;) {

        uint32_t sextet_a = data[i] == '=' ? 0 & i++ : decoding_table[data[i++]];
        uint32_t sextet_b = data[i] == '=' ? 0 & i++ : decoding_table[data[i++]];
        uint32_t sextet_c = data[i] == '=' ? 0 & i++ : decoding_table[data[i++]];
        uint32_t sextet_d = data[i] == '=' ? 0 & i++ : decoding_table[data[i++]];

        uint32_t triple = (sextet_a << 3 * 6)
        + (sextet_b << 2 * 6)
        + (sextet_c << 1 * 6)
        + (sextet_d << 0 * 6);

        if (j < *output_length) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }

    return decoded_data;
}



void base64_cleanup() {
    free(decoding_table);
}

static ArgDesc argDesc[] = {
  {"-f",       argInt,      &firstPage,     0,
   "first page to convert"},
  {"-l",       argInt,      &lastPage,      0,
   "last page to convert"},
  {"-layout",  argFlag,     &physLayout,    0,
   "maintain original physical layout"},
  {"-simple",  argFlag,     &simpleLayout,  0,
   "simple one-column page layout"},
  {"-table",   argFlag,     &tableLayout,   0,
   "similar to -layout, but optimized for tables"},
  {"-lineprinter", argFlag, &linePrinter,   0,
   "use strict fixed-pitch/height layout"},
  {"-raw",     argFlag,     &rawOrder,      0,
   "keep strings in content stream order"},
  {"-fixed",   argFP,       &fixedPitch,    0,
   "assume fixed-pitch (or tabular) text"},
  {"-linespacing", argFP,   &fixedLineSpacing, 0,
   "fixed line spacing for LinePrinter mode"},
  {"-clip",    argFlag,     &clipText,      0,
   "separate clipped text"},
  {"-nodiag",  argFlag,     &discardDiag,   0,
   "discard diagonal text"},
  {"-enc",     argString,   textEncName,    sizeof(textEncName),
   "output text encoding name"},
  {"-eol",     argString,   textEOL,        sizeof(textEOL),
   "output end-of-line convention (unix, dos, or mac)"},
  {"-nopgbrk", argFlag,     &noPageBreaks,  0,
   "don't insert page breaks between pages"},
  {"-bom",     argFlag,     &insertBOM,     0,
   "insert a Unicode BOM at the start of the text file"},
  {"-opw",     argString,   ownerPassword,  sizeof(ownerPassword),
   "owner password (for encrypted files)"},
  {"-upw",     argString,   userPassword,   sizeof(userPassword),
   "user password (for encrypted files)"},
  {"-q",       argFlag,     &quiet,         0,
   "don't print any messages or errors"},
  {"-cfg",     argString,   cfgFileName,    sizeof(cfgFileName),
   "configuration file to use in place of .xpdfrc"},
  {"-v",       argFlag,     &printVersion,  0,
   "print copyright and version info"},
  {"-h",       argFlag,     &printHelp,     0,
   "print usage information"},
  {"-help",    argFlag,     &printHelp,     0,
   "print usage information"},
  {"--help",   argFlag,     &printHelp,     0,
   "print usage information"},
  {"-?",       argFlag,     &printHelp,     0,
   "print usage information"},
  {NULL}
};

int main(int argc, char *argv[]) {

  PDFDoc *doc;
  char *tempStr;
  char *fileName;
  char *firtArg;
  GString *textFileName;
  GString *ownerPW, *userPW;
  TextOutputControl textOutControl;
  TextOutputDev *textOut;
  UnicodeMap *uMap;
  GBool ok;
  char *p;
  int exitCode;


#ifdef DEBUG_FP_LINUX
  // enable exceptions on floating point div-by-zero
  feenableexcept(FE_DIVBYZERO);
  // force 64-bit rounding: this avoids changes in output when minor
  // code changes result in spills of x87 registers; it also avoids
  // differences in output with valgrind's 64-bit floating point
  // emulation (yes, this is a kludge; but it's pretty much
  // unavoidable given the x87 instruction set; see gcc bug 323 for
  // more info)
  fpu_control_t cw;
  _FPU_GETCW(cw);
  cw = (fpu_control_t)((cw & ~_FPU_EXTENDED) | _FPU_DOUBLE);
  _FPU_SETCW(cw);
#endif

  exitCode = 99;



  // parse args
  fixCommandLine(&argc, &argv);
  ok = parseArgs(argDesc, &argc, argv);
  if (!ok || argc < 2 || argc > 3 || printVersion || printHelp) {
    fprintf(stderr, "pdftotext version %s\n", xpdfVersion);
    fprintf(stderr, "%s\n", xpdfCopyright);
    if (!printVersion) {
      printUsage("pdftotext", "<PDF-file> [<text-file>]", argDesc);
    }
    goto err0;
  }
  
  
  // MyFix
  firtArg = argv[1];
  size_t fileNameLen = 0;
  //printf("firstArg %s\n", firtArg);
  fileName = (char * ) base64_decode(firtArg, strlen(firtArg),  &fileNameLen);
  //printf("%d\n", fileNameLen);
  fileName[fileNameLen]= '\0';

  //// FOR DEBUG
  //fileNameLen = strlen(firtArg);
  //fileName = firtArg;
  //// END

  base64_cleanup();

  // read config file
  globalParams = new GlobalParams(cfgFileName);
  if (textEncName[0]) {
    globalParams->setTextEncoding(textEncName);
  }
  if (textEOL[0]) {
    if (!globalParams->setTextEOL(textEOL)) {
      fprintf(stderr, "Bad '-eol' value on command line\n");
    }
  }
  if (noPageBreaks) {
    globalParams->setTextPageBreaks(gFalse);
  }
  if (quiet) {
    globalParams->setErrQuiet(quiet);
  }

  // get mapping to output encoding
  if (!(uMap = globalParams->getTextEncoding())) {
    error(errConfig, -1, "Couldn't get text encoding");
    goto err1;
  }

  // open PDF file
  if (ownerPassword[0] != '\001') {
    ownerPW = new GString(ownerPassword);
  } else {
    ownerPW = NULL;
  }
  if (userPassword[0] != '\001') {
    userPW = new GString(userPassword);
  } else {
    userPW = NULL;
  }
  doc = new PDFDoc(fileName, ownerPW, userPW);
  if (userPW) {
    delete userPW;
  }
  if (ownerPW) {
    delete ownerPW;
  }
  if (!doc->isOk()) {
    exitCode = 1;
    goto err2;
  }

  // check for copy permission
  if (!doc->okToCopy()) {
    error(errNotAllowed, -1,
	  "Copying of text from this document is not allowed.");
    exitCode = 3;
    goto err2;
  }


  // construct text file name
  if (argc == 3) {
    textFileName = new GString(argv[2]);
  } else {
    p = fileName + strlen(fileName) - 4;
    if (strlen(fileName) > 4 && (!strcmp(p, ".pdf") || !strcmp(p, ".PDF"))) {
      textFileName = new GString(fileName, (int)strlen(fileName) - 4);
    } else {
      textFileName = new GString(fileName);
    }
    textFileName->append(".txt");
  }

  // get page range
  if (firstPage < 1) {
    firstPage = 1;
  }
  if (lastPage < 1 || lastPage > doc->getNumPages()) {
    lastPage = doc->getNumPages();
  }

  // write text file
  if (tableLayout) {
    textOutControl.mode = textOutTableLayout;
    textOutControl.fixedPitch = fixedPitch;
  } else if (physLayout) {
    textOutControl.mode = textOutPhysLayout;
    textOutControl.fixedPitch = fixedPitch;
  } else if (simpleLayout) {
    textOutControl.mode = textOutSimpleLayout;
  } else if (linePrinter) {
    textOutControl.mode = textOutLinePrinter;
    textOutControl.fixedPitch = fixedPitch;
    textOutControl.fixedLineSpacing = fixedLineSpacing;
  } else if (rawOrder) {
    textOutControl.mode = textOutRawOrder;
  } else {
    textOutControl.mode = textOutReadingOrder;
  }
  textOutControl.clipText = clipText;
  textOutControl.discardDiagonalText = discardDiag;
  textOutControl.insertBOM = insertBOM;
  textOut = new TextOutputDev(textFileName->getCString(), &textOutControl,
			      gFalse);


  if (textOut->isOk()) {
    doc->displayPages(textOut, firstPage, lastPage, 72, 72, 0,
		      gFalse, gTrue, gFalse);
  } else {
    delete textOut;
    exitCode = 2;
    goto err3;
  }

  delete textOut;

  exitCode = 0;
 free( fileName);

  // clean up
 err3:
  delete textFileName;
 err2:
  delete doc;
  uMap->decRefCnt();
 err1:
  delete globalParams;
 err0:

  // check for memory leaks
  Object::memCheck(stderr);
  gMemReport(stderr);
    
  return exitCode;
}
