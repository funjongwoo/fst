/*
  fst - An R-package for ultra fast storage and retrieval of datasets.
  Copyright (C) 2017, Mark AJ Klik

  BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  You can contact the author at :
  - fst source repository : https://github.com/fstPackage/fst
*/

#include <FastStore.h>
#include <FastStore_v1.h>

#include <Rcpp.h>
#include <iostream>
#include <fstream>
#include <R.h>
#include <Rinternals.h>

#include <character_v6.h>
#include <factor_v7.h>
#include <integer_v8.h>
#include <double_v9.h>
#include <logical_v10.h>

#include <compression.h>
#include <compressor.h>

// External libraries
#include "lz4.h"
#include "zstd.h"

using namespace std;
using namespace Rcpp;

#define BLOCKSIZE       16384               // number of bytes in default compression block
#define FST_VERSION     1                   // version number of the fst package
#define FST_FILE_ID     0xa91c12f8b245a71d  // identifies a fst file
#define TABLE_META_SIZE 24                  // size of table meta-data block

inline int FindKey(StringVector colNameList, String item)
{
  int index = -1;
  int found = 0;
  for (Rcpp::StringVector::iterator it = colNameList.begin(); it != colNameList.end(); ++it)
  {
    if (*it == item)
    {
      index = found;
      break;
    }
    ++found;
  }

  return index;
}


// Table metadata
//
//  NR OF BYTES            | TYPE               | VARIABLE NAME
//
//  4                      | int                | nrOfCols
//  4                      | int                | keyLength
//  4                      | unsigned int       | FST_VERSION
//  4                      | unsigned int       | nrOfChunksPerIndexRow
//
//  8                      | unsigned long long | FST_FILE_ID
//  8 * 8 (index rows)     | unsigned long long | chunkPos
//  8 * 8 (index rows)     | unsigned long long | chunkRows
//
//  4                      | unsigned int       | nrOfChunks
//  4 * keyLength          | int                | keyColPos
//  2 * nrOfCols           | unsigned short int | colTypes
//
//  Table columnar position data
//
//  8 * nrOfCols           | unsigned long long | positionData
//                         | char               | colNames

SEXP fstStore(String fileName, SEXP table, SEXP compression, Function serializer)
{
  SEXP colNames = Rf_getAttrib(table, R_NamesSymbol);
  SEXP keyNames = Rf_getAttrib(table, Rf_mkString("sorted"));


  if (!Rf_isInteger(compression))
  {
    ::Rf_error("Parameter compression should be an integer value between 0 and 100");
  }

  int compress = *INTEGER(compression);
  if ((compress < 0) | (compress > 100))
  {
    ::Rf_error("Parameter compression should be an integer value between 0 and 100");
  }

  // Meta on dataset
  int nrOfCols =  LENGTH(colNames);
  int keyLength = 0;
  if (!Rf_isNull(keyNames))
  {
    keyLength = LENGTH(keyNames);
  }

  if (nrOfCols == 0)
  {
    ::Rf_error("Your dataset needs at least one column.");
  }


  // Table meta information
  unsigned long long metaDataSize        = 156 + 4 * keyLength + 2 * nrOfCols;  // see index above
  char* metaDataBlock                    = new char[metaDataSize];

  int* p_nrOfCols                        = (int*) metaDataBlock;
  int* p_keyLength                       = (int*) &metaDataBlock[4];
  unsigned int* p_version                = (unsigned int*) &metaDataBlock[8];
  unsigned int* p_nrOfChunksPerIndexRow  = (unsigned int*) &metaDataBlock[12];
  unsigned long long* fstFileID          = (unsigned long long*) &metaDataBlock[16];

  // chunk index
  unsigned long long* chunkPos           = (unsigned long long*) &metaDataBlock[24];
  unsigned long long* chunkRows          = (unsigned long long*) &metaDataBlock[88];

  unsigned int* p_nrOfChunks             = (unsigned int*) &metaDataBlock[152];
  int*  keyColPos                        = (int*) &metaDataBlock[156];
  unsigned short int* colTypes           = (unsigned short int*) &metaDataBlock[156 + 4 * keyLength];


  // Find key column index numbers, if any
  if (keyLength != 0)
  {
    StringVector keyList(keyNames);

    for (int colSel = 0; colSel < keyLength; ++colSel)
    {
      keyColPos[colSel] = FindKey(colNames, keyList[colSel]);
    }
  }

  *p_nrOfCols              = nrOfCols;
  *p_keyLength             = keyLength;
  *p_version               = FST_VERSION;
  *p_nrOfChunksPerIndexRow = 1;
  *fstFileID               = FST_FILE_ID;
  *p_nrOfChunks            = 1;  // set to 0 if all reserved slots are used


  // data.frame code here for stability!
  SEXP firstCol = VECTOR_ELT(table, 0);
  int nrOfRows = LENGTH(firstCol);
  *chunkRows = (unsigned long long) nrOfRows;

  if (nrOfRows == 0)
  {
    delete[] metaDataBlock;
    ::Rf_error("The dataset contains no data.");
  }


  // Create file, set fast local buffer and open
  ofstream myfile;
  char ioBuf[4096];
  myfile.rdbuf()->pubsetbuf(ioBuf, 4096);  // workaround for memory leak in ofstream
  myfile.open(fileName.get_cstring(), ios::binary);

  if (myfile.fail())
  {
    delete[] metaDataBlock;
    myfile.close();
    ::Rf_error("There was an error creating the file. Please check for a correct filename.");
  }


  // Write table meta data
  myfile.write((char*)(metaDataBlock), metaDataSize);  // table meta data
  fdsWriteCharVec_v6(myfile, colNames, nrOfCols, 0);   // column names


  // TODO: Write table attributes here


  // TODO: Write column attributes here


  // Start writing chunk here


  // Row and column meta data
  unsigned long long *positionData = new unsigned long long[nrOfCols];  // column position index
  myfile.write((char*)(positionData), 8 * nrOfCols);   // file positions of column data


  SEXP colResult = NULL;

  // column data
  for (int colNr = 0; colNr < nrOfCols; ++colNr)
  {
    positionData[colNr] = myfile.tellp();  // current location
    SEXP colVec = VECTOR_ELT(table, colNr);  // column vector

    // Store attributes here if any
    // unsigned int attrBlockSize = SerializeObjectAttributes(ofstream &myfile, RObject rObject, serializer);

    switch (TYPEOF(colVec))
    {
      case STRSXP:
        colTypes[colNr] = 6;
        colResult = fdsWriteCharVec_v6(myfile, colVec, nrOfRows, compress);
        break;

      case INTSXP:
        if(Rf_isFactor(colVec))
        {
          colTypes[colNr] = 7;
          colResult = fdsWriteFactorVec_v7(myfile, colVec, nrOfRows, compress);
          break;
        }

        colTypes[colNr] = 8;
        colResult = fdsWriteIntVec_v8(myfile, colVec, nrOfRows, compress);
        break;

      case REALSXP:
        colTypes[colNr] = 9;
        colResult = fdsWriteRealVec_v9(myfile, colVec, nrOfRows, compress);
        break;

      case LGLSXP:
        colTypes[colNr] = 10;
        colResult = fdsWriteLogicalVec_v10(myfile, colVec, nrOfRows, compress);
        break;

      default:
        delete[] metaDataBlock;
        delete[] positionData;
        myfile.close();
        ::Rf_error("Unknown type found in column.");
    }
  }


  // update chunk position data
  *chunkPos = positionData[0] - 8 * nrOfCols;

  myfile.seekp(0);
  myfile.write((char*)(metaDataBlock), metaDataSize);  // table header

  myfile.seekp(*chunkPos);
  myfile.write((char*)(positionData), 8 * nrOfCols);  // chunk column index

  myfile.close();

  // cleanup
  delete[] metaDataBlock;
  delete[] positionData;

  return List::create(
    _["keyNames"] = keyNames,
    _["keyLength"] = keyLength,
    _["colResult"] = colResult,
    _["metaDataSize"] = metaDataSize);
}


// Read header information
inline unsigned int ReadHeader(ifstream &myfile, int &nrOfCols, int &keyLength, unsigned int &nrOfChunksPerIndexRow)
{
  // Get meta-information for table
  char tableMeta[TABLE_META_SIZE];
  myfile.read(tableMeta, TABLE_META_SIZE);

  if (!myfile)
  {
    myfile.close();
    ::Rf_error("Error reading file header, your fst file is incomplete or damaged.");
  }

  int* p_nrOfCols                       = (int*) tableMeta;
  int* p_keyLength                      = (int*) &tableMeta[4];
  unsigned int* p_version               = (unsigned int*) &tableMeta[8];
  unsigned int* p_nrOfChunksPerIndexRow = (unsigned int*) &tableMeta[12];
  unsigned long long* p_fstFileID       = (unsigned long long*) &tableMeta[16];

  // faster access
  nrOfCols                      = *p_nrOfCols;
  keyLength                     = *p_keyLength;
  unsigned int version          = *p_version;  // return parameter
  nrOfChunksPerIndexRow         = *p_nrOfChunksPerIndexRow;

  // Without a proper file ID, we may be looking at a fst v0.7.2 file format
  if (*p_fstFileID != FST_FILE_ID)
  {
    return 0;
  }

  // Compare file version with current
  if (version > FST_VERSION)
  {
    myfile.close();
    ::Rf_error("Incompatible fst file: file was created by a newer version of the fst package.");
  }

  return version;
}


List fstMeta(String fileName)
{
  // fst file stream using a stack buffer
  ifstream myfile;
  char ioBuf[4096];
  myfile.rdbuf()->pubsetbuf(ioBuf, 4096);
  myfile.open(fileName.get_cstring(), ios::binary);

  if (myfile.fail())
  {
    myfile.close();
    ::Rf_error("There was an error opening the fst file, please check for a correct path.");
  }

  // Read variables from fst file header
  int nrOfCols;
  int keyLength;
  unsigned int nrOfChunksReserved;

  unsigned int version = ReadHeader(myfile, nrOfCols, keyLength, nrOfChunksReserved);

  // We may be looking at a fst v0.7.2 file format
  if (version == 0)
  {
    // Close and reopen (slow and fst file should be resaved to avoid)
    myfile.close();
    return fstMeta_v1(fileName);  // scans further for safety
  }


  // Read table metadata
  unsigned long long dataMetaSize = 132 + 4 * keyLength + 2 * nrOfCols;  // see index above
  char* dataMeta = new char[dataMetaSize];
  myfile.read(dataMeta, dataMetaSize);

  // chunk index
  unsigned long long* chunkPos           = (unsigned long long*) dataMeta;
  unsigned long long* chunkRows          = (unsigned long long*) &dataMeta[64];
  unsigned int* p_nrOfChunks             = (unsigned int*) &dataMeta[128];
  int*  keyColPos                        = (int*) &dataMeta[132];
  unsigned short int* colTypes           = (unsigned short int*) &dataMeta[132 + 4 * keyLength];


  int nrOfRows = chunkRows[0];


  // Convert to integer vector
  IntegerVector colTypeVec(nrOfCols);
  for (int col = 0; col != nrOfCols; ++col)
  {
    colTypeVec[col] = colTypes[col];
  }


  // Read column names
  SEXP colNames;
  PROTECT(colNames = Rf_allocVector(STRSXP, nrOfCols));
  unsigned long long offset = dataMetaSize + TABLE_META_SIZE;
  fdsReadCharVec_v6(myfile, colNames, offset, 0, (unsigned int) nrOfCols, (unsigned int) nrOfCols);


  // Read block positions
  unsigned long long* blockPos = new unsigned long long[nrOfCols];
  myfile.read((char*) blockPos, nrOfCols * 8);  // nrOfCols file positions


  // cleanup
  delete[] blockPos;
  myfile.close();


  if (keyLength > 0)
  {
    SEXP keyNames;
    PROTECT(keyNames = Rf_allocVector(STRSXP, keyLength));
    for (int i = 0; i < keyLength; ++i)
    {
      SET_STRING_ELT(keyNames, i, STRING_ELT(colNames, keyColPos[i]));
    }

    IntegerVector keyColIndex(keyLength);
    for (int col = 0; col != keyLength; ++col)
    {
      keyColIndex[col] = keyColPos[col];
    }

    UNPROTECT(2);
    delete[] dataMeta;

    return List::create(
      _["nrOfCols"]        = nrOfCols,
      _["nrOfRows"]        = nrOfRows,
      _["fstVersion"]      = version,
      _["colTypeVec"]      = colTypeVec,
      _["keyColIndex"]     = keyColIndex,
      _["keyLength"]       = keyLength,
      _["keyNames"]        = keyNames,
      _["colNames"]        = colNames,
      _["nrOfChunks"]      = *p_nrOfChunks);
  }

  UNPROTECT(1);
  delete[] dataMeta;

  return List::create(
    _["nrOfCols"]        = nrOfCols,
    _["nrOfRows"]        = nrOfRows,
    _["fstVersion"]      = version,
    _["keyLength"]       = keyLength,
    _["colTypeVec"]      = colTypeVec,
    _["colNames"]        = colNames,
    _["nrOfChunks"]      = *p_nrOfChunks);
}


SEXP fstRead(SEXP fileName, SEXP columnSelection, SEXP startRow, SEXP endRow)
{
  // fst file stream using a stack buffer
  ifstream myfile;
  char ioBuf[4096];
  myfile.rdbuf()->pubsetbuf(ioBuf, 4096);
  myfile.open(CHAR(STRING_ELT(fileName, 0)), ios::binary);

  if (myfile.fail())
  {
    myfile.close();
    ::Rf_error("There was an error opening the fst file, please check for a correct path.");
  }


  // Read variables from fst file header
  int nrOfCols;
  int keyLength;
  unsigned int nrOfChunksPerIndexRow;

  unsigned int version = ReadHeader(myfile, nrOfCols, keyLength, nrOfChunksPerIndexRow);

  // We may be looking at a fst v0.7.2 file format
  if (version == 0)
  {
    // Close and reopen (slow: fst file should be resaved to avoid this overhead)
    myfile.close();
    return fstRead_v1(fileName, columnSelection, startRow, endRow);
  }


  // Read table metadata
  unsigned long long dataMetaSize = 132 + 4 * keyLength + 2 * nrOfCols;  // see index above
  char* dataMeta = new char[dataMetaSize];
  myfile.read(dataMeta, dataMetaSize);

  // chunk index
  unsigned long long* chunkPos           = (unsigned long long*) dataMeta;
  unsigned long long* chunkRows          = (unsigned long long*) &dataMeta[64];
  unsigned int* p_nrOfChunks             = (unsigned int*) &dataMeta[128];
  int*  keyColPos                        = (int*) &dataMeta[132];
  unsigned short int* colTypes           = (unsigned short int*) &dataMeta[132 + 4 * keyLength];


  // Check nrOfChunks
  if (*p_nrOfChunks > 1)
  {
    myfile.close();
    delete[] dataMeta;
    ::Rf_error("Multiple chunk read not implemented yet.");
  }


  // Read column names
  SEXP colNames;
  PROTECT(colNames = Rf_allocVector(STRSXP, nrOfCols));
  unsigned long long offset = dataMetaSize + TABLE_META_SIZE;
  fdsReadCharVec_v6(myfile, colNames, offset, 0, (unsigned int) nrOfCols, (unsigned int) nrOfCols);



  // TODO: read table attributes here


  // TODO: read column attributes here


  // Start reading chunk here. TODO: loop over chunks


  // Read block positions
  unsigned long long* blockPos = new unsigned long long[nrOfCols];
  myfile.read((char*) blockPos, nrOfCols * 8);  // nrOfCols file positions



  // Determine column selection
  int *colIndex;
  int nrOfSelect = LENGTH(columnSelection);

  if (Rf_isNull(columnSelection))
  {
    colIndex = new int[nrOfCols];

    for (int colNr = 0; colNr < nrOfCols; ++colNr)
    {
      colIndex[colNr] = colNr;
    }
    nrOfSelect = nrOfCols;
  }
  else  // determine column numbers of column names
  {
    colIndex = new int[nrOfSelect];
    int equal;
    for (int colSel = 0; colSel < nrOfSelect; ++colSel)
    {
      equal = -1;
      const char* str1 = CHAR(STRING_ELT(columnSelection, colSel));

      for (int colNr = 0; colNr < nrOfCols; ++colNr)
      {
        const char* str2 = CHAR(STRING_ELT(colNames, colNr));
        if (strcmp(str1, str2) == 0)
        {
          equal = colNr;
          break;
        }
      }

      if (equal == -1)
      {
        delete[] dataMeta;
        delete[] colIndex;
        delete[] blockPos;
        myfile.close();
        UNPROTECT(1);
        ::Rf_error("Selected column not found.");
      }

      colIndex[colSel] = equal;
    }
  }


  // Check range of selected rows
  int firstRow = INTEGER(startRow)[0] - 1;
  int nrOfRows = *chunkRows;  // TODO: check for row numbers > INT_MAX !!!

  if (firstRow >= nrOfRows || firstRow < 0)
  {
    delete[] dataMeta;
    delete[] colIndex;
    delete[] blockPos;
    myfile.close();
    UNPROTECT(1);

    if (firstRow < 0)
    {
      ::Rf_error("Parameter fromRow should have a positive value.");
    }

    ::Rf_error("Row selection is out of range.");
  }

  int length = nrOfRows - firstRow;


  // Determine vector length
  if (!Rf_isNull(endRow))
  {
    int lastRow = *INTEGER(endRow);

    if (lastRow <= firstRow)
    {
      delete[] dataMeta;
      delete[] colIndex;
      delete[] blockPos;
      myfile.close();
      UNPROTECT(1);
      ::Rf_error("Parameter 'lastRow' should be equal to or larger than parameter 'fromRow'.");
    }

    length = min(lastRow - firstRow, nrOfRows - firstRow);
  }

  // Vector of selected column names
  SEXP selectedNames;
  PROTECT(selectedNames = Rf_allocVector(STRSXP, nrOfSelect));

  SEXP resTable;
  PROTECT(resTable = Rf_allocVector(VECSXP, nrOfSelect));

  SEXP colInfo;
  PROTECT(colInfo = Rf_allocVector(VECSXP, nrOfSelect));

  for (int colSel = 0; colSel < nrOfSelect; ++colSel)
  {
    int colNr = colIndex[colSel];

    if (colNr < 0 || colNr >= nrOfCols)
    {
      delete[] dataMeta;
      delete[] colIndex;
      delete[] blockPos;
      myfile.close();
      UNPROTECT(4);
      ::Rf_error("Column selection is out of range.");
    }

    // Column name
    SEXP selName = STRING_ELT(colNames, colNr);
    SET_STRING_ELT(selectedNames, colSel, selName);

    unsigned long long pos = blockPos[colNr];

    SEXP singleColInfo;

    switch (colTypes[colNr])
    {
    // Character vector
      case 6:
        SEXP strVec;
        PROTECT(strVec = Rf_allocVector(STRSXP, length));
        singleColInfo = fdsReadCharVec_v6(myfile, strVec, pos, firstRow, length, nrOfRows);
        SET_VECTOR_ELT(resTable, colSel, strVec);
        UNPROTECT(1);
        break;

      // Integer vector
      case 8:
        SEXP intVec;
        PROTECT(intVec = Rf_allocVector(INTSXP, length));
        singleColInfo = fdsReadIntVec_v8(myfile, intVec, pos, firstRow, length, nrOfRows);
        SET_VECTOR_ELT(resTable, colSel, intVec);
        UNPROTECT(1);
        break;

      // Real vector
      case 9:
        SEXP realVec;
        PROTECT(realVec = Rf_allocVector(REALSXP, length));
        singleColInfo = fdsReadRealVec_v9(myfile, realVec, pos, firstRow, length, nrOfRows);
        SET_VECTOR_ELT(resTable, colSel, realVec);
        UNPROTECT(1);
        break;

      // Logical vector
      case 10:
        SEXP boolVec;
        PROTECT(boolVec = Rf_allocVector(LGLSXP, length));
        singleColInfo = fdsReadLogicalVec_v10(myfile, boolVec, pos, firstRow, length, nrOfRows);
        SET_VECTOR_ELT(resTable, colSel, boolVec);
        UNPROTECT(1);
        break;

      // Factor vector
      case 7:
        SEXP facVec;
        PROTECT(facVec = Rf_allocVector(INTSXP, length));
        singleColInfo = fdsReadFactorVec_v7(myfile, facVec, pos, firstRow, length, nrOfRows);
        SET_VECTOR_ELT(resTable, colSel, facVec);
        UNPROTECT(2);  // level string was also generated
        break;


      default:
        delete[] dataMeta;
        delete[] colIndex;
        delete[] blockPos;
        myfile.close();
        ::Rf_error("Unknown type found in column.");
    }

    SET_VECTOR_ELT(colInfo, colSel, singleColInfo);
  }

  myfile.close();

  // Generalize to full atributes
  Rf_setAttrib(resTable, R_NamesSymbol, selectedNames);

  int found = 0;
  for (int i = 0; i < keyLength; ++i)
  {
    for (int colSel = 0; colSel < nrOfSelect; ++colSel)
    {
      if (keyColPos[i] == colIndex[colSel])  // key present in result
      {
        ++found;
        break;
      }
    }
  }

  // Only when keys are present in result set
  if (found > 0)
  {
    SEXP keyNames;
    PROTECT(keyNames = Rf_allocVector(STRSXP, found));
    for (int i = 0; i < found; ++i)
    {
      SET_STRING_ELT(keyNames, i, STRING_ELT(colNames, keyColPos[i]));
    }

    // cleanup
    UNPROTECT(5);
    delete[] dataMeta;
    delete[] colIndex;
    delete[] blockPos;

    return List::create(
      _["keyNames"] = keyNames,
      _["found"] = found,
      _["resTable"] = resTable,
      _["selectedNames"] = selectedNames,
      _["colInfo"] = colInfo);
  }

  UNPROTECT(4);
  delete[] dataMeta;
  delete[] colIndex;
  delete[] blockPos;

  // Convert to integer vector
  IntegerVector keyColVec(keyLength + 1);
  for (int col = 0; col != keyLength; ++col)
  {
    keyColVec[col] = keyColPos[col];
  }


  return List::create(
    _["keyNames"] = R_NilValue,
    _["found"] = found,
    _["keyColVec"] = keyColVec,
    _["keyLength"] = keyLength,
    _["nrOfSelect"] = nrOfSelect,
    _["selectedNames"] = selectedNames,
    _["resTable"] = resTable,
    _["colInfo"] = colInfo);
}
