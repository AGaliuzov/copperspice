/***********************************************************************
*
* Copyright (c) 2012-2017 Barbara Geller
* Copyright (c) 2012-2017 Ansel Sermersheim
* Copyright (c) 2012-2016 Digia Plc and/or its subsidiary(-ies).
* Copyright (c) 2008-2012 Nokia Corporation and/or its subsidiary(-ies).
* All rights reserved.
*
* This file is part of CopperSpice.
*
* CopperSpice is free software. You can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public License
* version 2.1 as published by the Free Software Foundation.
*
* CopperSpice is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*
* <http://www.gnu.org/licenses/>.
*
***********************************************************************/

#include <qbytearray.h>
#include <qbytearraymatcher.h>
#include <qtools_p.h>
#include <qstring.h>
#include <qlist.h>
#include <qlocale.h>
#include <qlocale_p.h>
#include <qscopedpointer.h>
#include <qdatastream.h>

#ifndef QT_NO_COMPRESS
#include <zlib.h>
#endif

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>

#define IS_RAW_DATA(d) ((d)->offset != sizeof(QByteArrayData))

QT_BEGIN_NAMESPACE

int qFindByteArray(
   const char *haystack0, int haystackLen, int from,
   const char *needle0, int needleLen);

int qAllocMore(int alloc, int extra)
{
   Q_ASSERT(alloc >= 0 && extra >= 0);
   Q_ASSERT(alloc < (1 << 30) - extra);

   unsigned nalloc = alloc + extra;

   // Round up to next power of 2

   // Assuming container is growing, always overshoot
   //--nalloc;

   nalloc |= nalloc >> 1;
   nalloc |= nalloc >> 2;
   nalloc |= nalloc >> 4;
   nalloc |= nalloc >> 8;
   nalloc |= nalloc >> 16;
   ++nalloc;

   Q_ASSERT(nalloc > unsigned(alloc + extra));

   return nalloc - extra;
}

/*****************************************************************************
  Safe and portable C string functions; extensions to standard string.h
 *****************************************************************************/

char *qstrdup(const char *src)
{
   if (!src) {
      return 0;
   }
   char *dst = new char[strlen(src) + 1];
   return qstrcpy(dst, src);
}

char *qstrcpy(char *dst, const char *src)
{
   if (!src) {
      return 0;
   }

   return strcpy(dst, src);

}

char *qstrncpy(char *dst, const char *src, uint len)
{
   if (!src || !dst) {
      return 0;
   }

   strncpy(dst, src, len);

   if (len > 0) {
      dst[len - 1] = '\0';
   }
   return dst;
}

int qstrcmp(const char *str1, const char *str2)
{
   return (str1 && str2) ? strcmp(str1, str2)
          : (str1 ? 1 : (str2 ? -1 : 0));
}

int qstricmp(const char *str1, const char *str2)
{
   const uchar *s1 = reinterpret_cast<const uchar *>(str1);
   const uchar *s2 = reinterpret_cast<const uchar *>(str2);
   int res;
   uchar c;
   if (!s1 || !s2) {
      return s1 ? 1 : (s2 ? -1 : 0);
   }
   for (; !(res = (c = QChar::toLower((ushort) * s1)) - QChar::toLower((ushort) * s2)); s1++, s2++)
      if (!c) {                              // strings are equal
         break;
      }
   return res;
}

int qstrnicmp(const char *str1, const char *str2, uint len)
{
   const uchar *s1 = reinterpret_cast<const uchar *>(str1);
   const uchar *s2 = reinterpret_cast<const uchar *>(str2);
   int res;
   uchar c;
   if (!s1 || !s2) {
      return s1 ? 1 : (s2 ? -1 : 0);
   }
   for (; len--; s1++, s2++) {
      if ((res = (c = QChar::toLower((ushort) * s1)) - QChar::toLower((ushort) * s2))) {
         return res;
      }
      if (!c) {                              // strings are equal
         break;
      }
   }
   return 0;
}

/*!
    \internal
 */
int qstrcmp(const QByteArray &str1, const char *str2)
{
   if (!str2) {
      return str1.isEmpty() ? 0 : +1;
   }

   const char *str1data = str1.constData();
   const char *str1end = str1data + str1.length();
   for ( ; str1data < str1end && *str2; ++str1data, ++str2) {
      int diff = int(uchar(*str1data)) - uchar(*str2);
      if (diff)
         // found a difference
      {
         return diff;
      }
   }

   // Why did we stop?
   if (*str2 != '\0')
      // not the null, so we stopped because str1 is shorter
   {
      return -1;
   }
   if (str1data < str1end)
      // we haven't reached the end, so str1 must be longer
   {
      return +1;
   }
   return 0;
}

/*!
    \internal
 */
int qstrcmp(const QByteArray &str1, const QByteArray &str2)
{
   int l1 = str1.length();
   int l2 = str2.length();
   int ret = memcmp(str1.constData(), str2.constData(), qMin(l1, l2));
   if (ret != 0) {
      return ret;
   }

   // they matched qMin(l1, l2) bytes
   // so the longer one is lexically after the shorter one
   return l1 - l2;
}


static const quint16 crc_tbl[16] = {
   0x0000, 0x1081, 0x2102, 0x3183,
   0x4204, 0x5285, 0x6306, 0x7387,
   0x8408, 0x9489, 0xa50a, 0xb58b,
   0xc60c, 0xd68d, 0xe70e, 0xf78f
};

quint16 qChecksum(const char *data, uint len)
{
   quint16 crc = 0xffff;
   uchar c;
   const uchar *p = reinterpret_cast<const uchar *>(data);
   while (len--) {
      c = *p++;
      crc = ((crc >> 4) & 0x0fff) ^ crc_tbl[((crc ^ c) & 15)];
      c >>= 4;
      crc = ((crc >> 4) & 0x0fff) ^ crc_tbl[((crc ^ c) & 15)];
   }
   return ~crc & 0xffff;
}

#ifndef QT_NO_COMPRESS
QByteArray qCompress(const uchar *data, int nbytes, int compressionLevel)
{
   if (nbytes == 0) {
      return QByteArray(4, '\0');
   }
   if (!data) {
      qWarning("qCompress: Data is null");
      return QByteArray();
   }
   if (compressionLevel < -1 || compressionLevel > 9) {
      compressionLevel = -1;
   }

   ulong len = nbytes + nbytes / 100 + 13;
   QByteArray bazip;
   int res;
   do {
      bazip.resize(len + 4);
      res = ::compress2((uchar *)bazip.data() + 4, &len, (uchar *)data, nbytes, compressionLevel);

      switch (res) {
         case Z_OK:
            bazip.resize(len + 4);
            bazip[0] = (nbytes & 0xff000000) >> 24;
            bazip[1] = (nbytes & 0x00ff0000) >> 16;
            bazip[2] = (nbytes & 0x0000ff00) >> 8;
            bazip[3] = (nbytes & 0x000000ff);
            break;
         case Z_MEM_ERROR:
            qWarning("qCompress: Z_MEM_ERROR: Not enough memory");
            bazip.resize(0);
            break;
         case Z_BUF_ERROR:
            len *= 2;
            break;
      }
   } while (res == Z_BUF_ERROR);

   return bazip;
}
#endif

#ifndef QT_NO_COMPRESS
QByteArray qUncompress(const uchar *data, int nbytes)
{
   if (!data) {
      qWarning("qUncompress: Data is null");
      return QByteArray();
   }
   if (nbytes <= 4) {
      if (nbytes < 4 || (data[0] != 0 || data[1] != 0 || data[2] != 0 || data[3] != 0)) {
         qWarning("qUncompress: Input data is corrupted");
      }
      return QByteArray();
   }
   ulong expectedSize = (data[0] << 24) | (data[1] << 16) |
                        (data[2] <<  8) | (data[3]      );
   ulong len = qMax(expectedSize, 1ul);
   QScopedPointer<QByteArray::Data, QScopedPointerPodDeleter> d;

   forever {
      ulong alloc = len;
      if (len  >= (1u << 31u) - sizeof(QByteArray::Data))
      {
         //QByteArray does not support that huge size anyway.
         qWarning("qUncompress: Input data is corrupted");
         return QByteArray();
      }
      QByteArray::Data *p = static_cast<QByteArray::Data *>(::realloc(d.data(), sizeof(QByteArray::Data) + alloc + 1));
      if (!p)
      {
         // Qt 5 - throw an exception
         qWarning("qUncompress: could not allocate enough memory to uncompress data");
         return QByteArray();
      }
      d.take(); // realloc was successful
      d.reset(p);
      d->offset = sizeof(QByteArrayData);

      int res = ::uncompress((uchar *)d->data(), &len,
      (uchar *)data + 4, nbytes - 4);

      switch (res)
      {
         case Z_OK:
            if (len != alloc) {
               if (len  >= (1u << 31u) - sizeof(QByteArray::Data)) {
                  //QByteArray does not support that huge size anyway.
                  qWarning("qUncompress: Input data is corrupted");
                  return QByteArray();
               }
               QByteArray::Data *p = static_cast<QByteArray::Data *>(::realloc(d.data(), sizeof(QByteArray::Data) + len + 1));
               if (!p) {
                  // Qt 5 - throw an exception
                  qWarning("qUncompress: could not allocate enough memory to uncompress data");
                  return QByteArray();
               }
               d.take(); // realloc was successful
               d.reset(p);
            }
            d->ref.initializeOwned();
            d->size = len;
            d->alloc = uint(len) + 1u;
            d->capacityReserved = false;
            d->offset = sizeof(QByteArrayData);
            d->data()[len] = 0;

            {
               QByteArrayDataPtr dataPtr = { d.take() };
               return QByteArray(dataPtr);
            }

         case Z_MEM_ERROR:
            qWarning("qUncompress: Z_MEM_ERROR: Not enough memory");
            return QByteArray();

         case Z_BUF_ERROR:
            len *= 2;
            continue;

         case Z_DATA_ERROR:
            qWarning("qUncompress: Z_DATA_ERROR: Input data is corrupted");
            return QByteArray();
      }
   }
}
#endif

static inline bool qIsUpper(char c)
{
   return c >= 'A' && c <= 'Z';
}

static inline char qToLower(char c)
{
   if (c >= 'A' && c <= 'Z') {
      return c - 'A' + 'a';
   } else {
      return c;
   }
}

QByteArray &QByteArray::operator=(const QByteArray &other)
{
   other.d->ref.ref();
   if (!d->ref.deref()) {
      Data::deallocate(d);
   }
   d = other.d;
   return *this;
}

QByteArray &QByteArray::operator=(const char *str)
{
   Data *x;
   if (!str) {
      x = Data::sharedNull();
   } else if (!*str) {
      x = Data::allocate(0);
   } else {
      int len = strlen(str);
      if (d->ref.isShared() || uint(len) + 1u > d->alloc
            || (len < d->size && uint(len) + 1u < uint(d->alloc >> 1))) {
         reallocData(uint(len) + 1u, d->detachFlags());
      }
      x = d;
      memcpy(x->data(), str, uint(len) + 1u); // include null terminator
      x->size = len;
   }
   x->ref.ref();
   if (!d->ref.deref()) {
      Data::deallocate(d);
   }
   d = x;
   return *this;
}

void QByteArray::truncate(int pos)
{
   if (pos < d->size) {
      resize(pos);
   }
}

void QByteArray::chop(int n)
{
   if (n > 0) {
      resize(d->size - n);
   }
}

QByteArray::QByteArray(const char *data, int size)
{
   if (!data) {
      d = Data::sharedNull();
   } else {
      if (size < 0) {
         size = strlen(data);
      }
      if (!size) {
         d = Data::allocate(0);
      } else {
         d = Data::allocate(uint(size) + 1u);
         Q_CHECK_PTR(d);
         d->size = size;
         memcpy(d->data(), data, size);
         d->data()[size] = '\0';
      }
   }
}


QByteArray::QByteArray(int size, char ch)
{
   if (size <= 0) {
      d = Data::allocate(0);
   } else {
      d = Data::allocate(uint(size) + 1u);
      Q_CHECK_PTR(d);
      d->size = size;
      memset(d->data(), ch, size);
      d->data()[size] = '\0';
   }
}

/*!
    \internal

    Constructs a byte array of size \a size with uninitialized contents.
*/

QByteArray::QByteArray(int size, Qt::Initialization)
{
   d = Data::allocate(uint(size) + 1u);
   Q_CHECK_PTR(d);
   d->size = size;
   d->data()[size] = '\0';
}

/*!
    Sets the size of the byte array to \a size bytes.

    If \a size is greater than the current size, the byte array is
    extended to make it \a size bytes with the extra bytes added to
    the end. The new bytes are uninitialized.

    If \a size is less than the current size, bytes are removed from
    the end.

    \sa size(), truncate()
*/
void QByteArray::resize(int size)
{
   if (size < 0) {
      size = 0;
   }

   if (IS_RAW_DATA(d) && !d->ref.isShared() && size < d->size) {
      d->size = size;
      return;
   }

   if (size == 0 && !d->capacityReserved) {
      Data *x = Data::allocate(0);
      if (!d->ref.deref()) {
         Data::deallocate(d);
      }
      d = x;
   } else if (d->size == 0 && d->ref.isStatic()) {
      //
      // Optimize the idiom:
      //    QByteArray a;
      //    a.resize(sz);
      //    ...
      // which is used in place of the Qt 3 idiom:
      //    QByteArray a(sz);
      //
      Data *x = Data::allocate(uint(size) + 1u);
      Q_CHECK_PTR(x);
      x->size = size;
      x->data()[size] = '\0';
      d = x;
   } else {
      if (d->ref.isShared() || uint(size) + 1u > d->alloc
            || (!d->capacityReserved && size < d->size
                && uint(size) + 1u < uint(d->alloc >> 1))) {
         reallocData(uint(size) + 1u, d->detachFlags() | Data::Grow);
      }
      if (d->alloc) {
         d->size = size;
         d->data()[size] = '\0';
      }
   }
}


QByteArray &QByteArray::fill(char ch, int size)
{
   resize(size < 0 ? d->size : size);
   if (d->size) {
      memset(d->data(), ch, d->size);
   }
   return *this;
}

void QByteArray::reallocData(uint alloc, Data::AllocationOptions options)
{
   if (d->ref.isShared() || IS_RAW_DATA(d)) {
      Data *x = Data::allocate(alloc, options);
      Q_CHECK_PTR(x);
      x->size = qMin(int(alloc) - 1, d->size);
      ::memcpy(x->data(), d->data(), x->size);
      x->data()[x->size] = '\0';
      if (!d->ref.deref()) {
         Data::deallocate(d);
      }
      d = x;
   } else {
      if (options & Data::Grow) {
         alloc = qAllocMore(alloc, sizeof(Data));
      }
      Data *x = static_cast<Data *>(::realloc(d, sizeof(Data) + alloc));
      Q_CHECK_PTR(x);
      x->alloc = alloc;
      x->capacityReserved = (options & Data::CapacityReserved) ? 1 : 0;
      d = x;
   }
}

void QByteArray::expand(int i)
{
   resize(qMax(i + 1, d->size));
}

QByteArray QByteArray::nulTerminated() const
{
   // is this fromRawData?
   if (!IS_RAW_DATA(d)) {
      return *this;   // no, then we're sure we're zero terminated
   }

   QByteArray copy(*this);
   copy.detach();
   return copy;
}


QByteArray &QByteArray::prepend(const QByteArray &ba)
{
   if (d->size == 0 && d->ref.isStatic() && !IS_RAW_DATA(ba.d)) {
      *this = ba;
   } else if (ba.d->size != 0) {
      QByteArray tmp = *this;
      *this = ba;
      append(tmp);
   }
   return *this;
}


QByteArray &QByteArray::prepend(const char *str)
{
   return prepend(str, qstrlen(str));
}


QByteArray &QByteArray::prepend(const char *str, int len)
{
   if (str) {
      if (d->ref.isShared() || uint(d->size + len) + 1u > d->alloc) {
         reallocData(uint(d->size + len) + 1u, d->detachFlags() | Data::Grow);
      }
      memmove(d->data() + len, d->data(), d->size);
      memcpy(d->data(), str, len);
      d->size += len;
      d->data()[d->size] = '\0';
   }
   return *this;
}


QByteArray &QByteArray::prepend(char ch)
{
   if (d->ref.isShared() || uint(d->size) + 2u > d->alloc) {
      reallocData(uint(d->size) + 2u, d->detachFlags() | Data::Grow);
   }
   memmove(d->data() + 1, d->data(), d->size);
   d->data()[0] = ch;
   ++d->size;
   d->data()[d->size] = '\0';
   return *this;
}

QByteArray &QByteArray::append(const QByteArray &ba)
{
   if (d->size == 0 && d->ref.isStatic() && !IS_RAW_DATA(ba.d)) {
      *this = ba;
   } else if (ba.d->size != 0) {
      if (d->ref.isShared() || uint(d->size + ba.d->size) + 1u > d->alloc) {
         reallocData(uint(d->size + ba.d->size) + 1u, d->detachFlags() | Data::Grow);
      }
      memcpy(d->data() + d->size, ba.d->data(), ba.d->size);
      d->size += ba.d->size;
      d->data()[d->size] = '\0';
   }
   return *this;
}

QByteArray &QByteArray::append(const char *str)
{
   if (str) {
      int len = strlen(str);
      if (d->ref.isShared() || uint(d->size + len) + 1u > d->alloc) {
         reallocData(uint(d->size + len) + 1u, d->detachFlags() | Data::Grow);
      }
      memcpy(d->data() + d->size, str, len + 1); // include null terminator
      d->size += len;
   }
   return *this;
}

QByteArray &QByteArray::append(const char *str, int len)
{
   if (len < 0) {
      len = qstrlen(str);
   }
   if (str && len) {
      if (d->ref.isShared() || uint(d->size + len) + 1u > d->alloc) {
         reallocData(uint(d->size + len) + 1u, d->detachFlags() | Data::Grow);
      }
      memcpy(d->data() + d->size, str, len); // include null terminator
      d->size += len;
      d->data()[d->size] = '\0';
   }
   return *this;
}

QByteArray &QByteArray::append(char ch)
{
   if (d->ref.isShared() || uint(d->size) + 2u > d->alloc) {
      reallocData(uint(d->size) + 2u, d->detachFlags() | Data::Grow);
   }
   d->data()[d->size++] = ch;
   d->data()[d->size] = '\0';
   return *this;
}

/*!
  \internal
  Inserts \a len bytes from the array \a arr at position \a pos and returns a
  reference the modified byte array.
*/
static inline QByteArray &qbytearray_insert(QByteArray *ba,
      int pos, const char *arr, int len)
{
   Q_ASSERT(pos >= 0);

   if (pos < 0 || len <= 0 || arr == 0) {
      return *ba;
   }

   int oldsize = ba->size();
   ba->resize(qMax(pos, oldsize) + len);
   char *dst = ba->data();
   if (pos > oldsize) {
      ::memset(dst + oldsize, 0x20, pos - oldsize);
   } else {
      ::memmove(dst + pos + len, dst + pos, oldsize - pos);
   }
   memcpy(dst + pos, arr, len);
   return *ba;
}


QByteArray &QByteArray::insert(int i, const QByteArray &ba)
{
   QByteArray copy(ba);
   return qbytearray_insert(this, i, copy.d->data(), copy.d->size);
}

QByteArray &QByteArray::insert(int i, const char *str)
{
   return qbytearray_insert(this, i, str, qstrlen(str));
}


QByteArray &QByteArray::insert(int i, const char *str, int len)
{
   return qbytearray_insert(this, i, str, len);
}

QByteArray &QByteArray::insert(int i, char ch)
{
   return qbytearray_insert(this, i, &ch, 1);
}

QByteArray &QByteArray::remove(char c)
{
   if (d->size == 0) {
      return *this;
   }

   QByteArray result(d->size, Qt::Uninitialized);
   const char *from    = d->data();
   const char *fromend = from + d->size;

   int outc = 0;
   char *to = result.d->data();

   while (from < fromend) {
      char temp = *from;

      if (temp != c) {
         *to = temp;
         ++to;
         ++outc;
      }

      ++from;
      if (temp ==  0) {
         break;
      }
   }

   result.resize(outc);
   *this = result;

   return *this;
}

QByteArray &QByteArray::remove(int pos, int len)
{
   if (len <= 0  || pos >= d->size || pos < 0) {
      return *this;
   }
   detach();

   if (pos + len >= d->size) {
      resize(pos);

   } else {
      memmove(d->data() + pos, d->data() + pos + len, d->size - pos - len);
      resize(d->size - len);
   }
   return *this;
}


QByteArray &QByteArray::replace(int pos, int len, const QByteArray &after)
{
   if (len == after.d->size && (pos + len <= d->size)) {
      detach();
      memmove(d->data() + pos, after.d->data(), len * sizeof(char));
      return *this;
   } else {
      QByteArray copy(after);
      // ### optimize me
      remove(pos, len);
      return insert(pos, copy);
   }
}

QByteArray &QByteArray::replace(int pos, int len, const char *after)
{
   return replace(pos, len, after, qstrlen(after));
}

QByteArray &QByteArray::replace(int pos, int len, const char *after, int alen)
{
   if (len == alen && (pos + len <= d->size)) {
      detach();
      memcpy(d->data() + pos, after, len * sizeof(char));
      return *this;
   } else {
      remove(pos, len);
      return qbytearray_insert(this, pos, after, alen);
   }
}

// ### optimize all other replace method, by offering
// QByteArray::replace(const char *before, int blen, const char *after, int alen)

QByteArray &QByteArray::replace(const QByteArray &before, const QByteArray &after)
{
   if (isNull() || before.d == after.d) {
      return *this;
   }

   QByteArray aft = after;
   if (after.d == d) {
      aft.detach();
   }

   return replace(before.constData(), before.size(), aft.constData(), aft.size());
}


QByteArray &QByteArray::replace(const char *c, const QByteArray &after)
{
   QByteArray aft = after;
   if (after.d == d) {
      aft.detach();
   }

   return replace(c, qstrlen(c), aft.constData(), aft.size());
}

QByteArray &QByteArray::replace(const char *before, int bsize, const char *after, int asize)
{
   if (isNull() || (before == after && bsize == asize)) {
      return *this;
   }

   // protect against before or after being part of this
   const char *a = after;
   const char *b = before;
   if (after >= d->data() && after < d->data() + d->size) {
      char *copy = (char *)malloc(asize);
      Q_CHECK_PTR(copy);
      memcpy(copy, after, asize);
      a = copy;
   }
   if (before >= d->data() && before < d->data() + d->size) {
      char *copy = (char *)malloc(bsize);
      Q_CHECK_PTR(copy);
      memcpy(copy, before, bsize);
      b = copy;
   }

   QByteArrayMatcher matcher(before, bsize);
   int index = 0;
   int len = d->size;
   char *d = data();

   if (bsize == asize) {
      if (bsize) {
         while ((index = matcher.indexIn(*this, index)) != -1) {
            memcpy(d + index, after, asize);
            index += bsize;
         }
      }
   } else if (asize < bsize) {
      uint to = 0;
      uint movestart = 0;
      uint num = 0;
      while ((index = matcher.indexIn(*this, index)) != -1) {
         if (num) {
            int msize = index - movestart;
            if (msize > 0) {
               memmove(d + to, d + movestart, msize);
               to += msize;
            }
         } else {
            to = index;
         }
         if (asize) {
            memcpy(d + to, after, asize);
            to += asize;
         }
         index += bsize;
         movestart = index;
         num++;
      }
      if (num) {
         int msize = len - movestart;
         if (msize > 0) {
            memmove(d + to, d + movestart, msize);
         }
         resize(len - num * (bsize - asize));
      }
   } else {
      // the most complex case. We don't want to lose performance by doing repeated
      // copies and reallocs of the string.
      while (index != -1) {
         uint indices[4096];
         uint pos = 0;
         while (pos < 4095) {
            index = matcher.indexIn(*this, index);
            if (index == -1) {
               break;
            }
            indices[pos++] = index;
            index += bsize;
            // avoid infinite loop
            if (!bsize) {
               index++;
            }
         }
         if (!pos) {
            break;
         }

         // we have a table of replacement positions, use them for fast replacing
         int adjust = pos * (asize - bsize);
         // index has to be adjusted in case we get back into the loop above.
         if (index != -1) {
            index += adjust;
         }
         int newlen = len + adjust;
         int moveend = len;
         if (newlen > len) {
            resize(newlen);
            len = newlen;
         }
         d = this->d->data();

         while (pos) {
            pos--;
            int movestart = indices[pos] + bsize;
            int insertstart = indices[pos] + pos * (asize - bsize);
            int moveto = insertstart + asize;
            memmove(d + moveto, d + movestart, (moveend - movestart));
            if (asize) {
               memcpy(d + insertstart, after, asize);
            }
            moveend = movestart - bsize;
         }
      }
   }

   if (a != after) {
      ::free((char *)a);
   }
   if (b != before) {
      ::free((char *)b);
   }


   return *this;
}

QByteArray &QByteArray::replace(char before, const QByteArray &after)
{
   char b[2] = { before, '\0' };
   QByteArray cb = fromRawData(b, 1);
   return replace(cb, after);
}

QByteArray &QByteArray::replace(char before, char after)
{
   if (d->size) {
      char *i = data();
      char *e = i + d->size;
      for (; i != e; ++i)
         if (*i == before) {
            * i = after;
         }
   }
   return *this;
}

QList<QByteArray> QByteArray::split(char sep) const
{
   QList<QByteArray> list;
   int start = 0;
   int end;
   while ((end = indexOf(sep, start)) != -1) {
      list.append(mid(start, end - start));
      start = end + 1;
   }
   list.append(mid(start));
   return list;
}

QByteArray QByteArray::repeated(int times) const
{
   if (d->size == 0) {
      return *this;
   }

   if (times <= 1) {
      if (times == 1) {
         return *this;
      }
      return QByteArray();
   }

   const int resultSize = times * d->size;

   QByteArray result;
   result.reserve(resultSize);
   if (result.d->alloc != uint(resultSize) + 1u) {
      return QByteArray();   // not enough memory
   }

   memcpy(result.d->data(), d->data(), d->size);

   int sizeSoFar = d->size;
   char *end = result.d->data() + sizeSoFar;

   const int halfResultSize = resultSize >> 1;
   while (sizeSoFar <= halfResultSize) {
      memcpy(end, result.d->data(), sizeSoFar);
      end += sizeSoFar;
      sizeSoFar <<= 1;
   }
   memcpy(end, result.d->data(), resultSize - sizeSoFar);
   result.d->data()[resultSize] = '\0';
   result.d->size = resultSize;
   return result;
}

#define REHASH(a) \
    if (ol_minus_1 < sizeof(uint) * CHAR_BIT) \
        hashHaystack -= (a) << ol_minus_1; \
    hashHaystack <<= 1

int QByteArray::indexOf(const QByteArray &ba, int from) const
{
   const int ol = ba.d->size;
   if (ol == 0) {
      return from;
   }
   if (ol == 1) {
      return indexOf(*ba.d->data(), from);
   }

   const int l = d->size;
   if (from > d->size || ol + from > l) {
      return -1;
   }

   return qFindByteArray(d->data(), d->size, from, ba.d->data(), ol);
}

int QByteArray::indexOf(const char *c, int from) const
{
   const int ol = qstrlen(c);
   if (ol == 1) {
      return indexOf(*c, from);
   }

   const int l = d->size;
   if (from > d->size || ol + from > l) {
      return -1;
   }
   if (ol == 0) {
      return from;
   }

   return qFindByteArray(d->data(), d->size, from, c, ol);
}

int QByteArray::indexOf(char ch, int from) const
{
   if (from < 0) {
      from = qMax(from + d->size, 0);
   }
   if (from < d->size) {
      const char *n = d->data() + from - 1;
      const char *e = d->data() + d->size;
      while (++n != e)
         if (*n == ch) {
            return  n - d->data();
         }
   }
   return -1;
}

static int lastIndexOfHelper(const char *haystack, int l, const char *needle, int ol, int from)
{
   int delta = l - ol;
   if (from < 0) {
      from = delta;
   }
   if (from < 0 || from > l) {
      return -1;
   }
   if (from > delta) {
      from = delta;
   }

   const char *end = haystack;
   haystack += from;
   const uint ol_minus_1 = ol - 1;
   const char *n = needle + ol_minus_1;
   const char *h = haystack + ol_minus_1;
   uint hashNeedle = 0, hashHaystack = 0;
   int idx;
   for (idx = 0; idx < ol; ++idx) {
      hashNeedle = ((hashNeedle << 1) + * (n - idx));
      hashHaystack = ((hashHaystack << 1) + * (h - idx));
   }
   hashHaystack -= *haystack;
   while (haystack >= end) {
      hashHaystack += *haystack;
      if (hashHaystack == hashNeedle && memcmp(needle, haystack, ol) == 0) {
         return haystack - end;
      }
      --haystack;
      REHASH(*(haystack + ol));
   }
   return -1;

}

int QByteArray::lastIndexOf(const QByteArray &ba, int from) const
{
   const int ol = ba.d->size;
   if (ol == 1) {
      return lastIndexOf(*ba.d->data(), from);
   }

   return lastIndexOfHelper(d->data(), d->size, ba.d->data(), ol, from);
}

int QByteArray::lastIndexOf(const char *str, int from) const
{
   const int ol = qstrlen(str);
   if (ol == 1) {
      return lastIndexOf(*str, from);
   }

   return lastIndexOfHelper(d->data(), d->size, str, ol, from);
}


int QByteArray::lastIndexOf(char ch, int from) const
{
   if (from < 0) {
      from += d->size;
   } else if (from > d->size) {
      from = d->size - 1;
   }
   if (from >= 0) {
      const char *b = d->data();
      const char *n = d->data() + from + 1;
      while (n-- != b)
         if (*n == ch) {
            return  n - b;
         }
   }
   return -1;
}

int QByteArray::count(const QByteArray &ba) const
{
   int num = 0;
   int i = -1;
   if (d->size > 500 && ba.d->size > 5) {
      QByteArrayMatcher matcher(ba);
      while ((i = matcher.indexIn(*this, i + 1)) != -1) {
         ++num;
      }
   } else {
      while ((i = indexOf(ba, i + 1)) != -1) {
         ++num;
      }
   }
   return num;
}

int QByteArray::count(const char *str) const
{
   return count(fromRawData(str, qstrlen(str)));
}

int QByteArray::count(char ch) const
{
   int num = 0;
   const char *i = d->data() + d->size;
   const char *b = d->data();
   while (i != b)
      if (*--i == ch) {
         ++num;
      }
   return num;
}

/*! \fn int QByteArray::count() const

    \overload

    Same as size().
*/

/*!
    Returns true if this byte array starts with byte array \a ba;
    otherwise returns false.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 25

    \sa endsWith(), left()
*/
bool QByteArray::startsWith(const QByteArray &ba) const
{
   if (d == ba.d || ba.d->size == 0) {
      return true;
   }
   if (d->size < ba.d->size) {
      return false;
   }
   return memcmp(d->data(), ba.d->data(), ba.d->size) == 0;
}

/*! \overload

    Returns true if this byte array starts with string \a str;
    otherwise returns false.
*/
bool QByteArray::startsWith(const char *str) const
{
   if (!str || !*str) {
      return true;
   }
   int len = strlen(str);
   if (d->size < len) {
      return false;
   }
   return qstrncmp(d->data(), str, len) == 0;
}

/*! \overload

    Returns true if this byte array starts with character \a ch;
    otherwise returns false.
*/
bool QByteArray::startsWith(char ch) const
{
   if (d->size == 0) {
      return false;
   }
   return d->data()[0] == ch;
}

/*!
    Returns true if this byte array ends with byte array \a ba;
    otherwise returns false.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 26

    \sa startsWith(), right()
*/
bool QByteArray::endsWith(const QByteArray &ba) const
{
   if (d == ba.d || ba.d->size == 0) {
      return true;
   }
   if (d->size < ba.d->size) {
      return false;
   }
   return memcmp(d->data() + d->size - ba.d->size, ba.d->data(), ba.d->size) == 0;
}

/*! \overload

    Returns true if this byte array ends with string \a str; otherwise
    returns false.
*/
bool QByteArray::endsWith(const char *str) const
{
   if (!str || !*str) {
      return true;
   }
   int len = strlen(str);
   if (d->size < len) {
      return false;
   }
   return qstrncmp(d->data() + d->size - len, str, len) == 0;
}

/*! \overload

    Returns true if this byte array ends with character \a ch;
    otherwise returns false.
*/
bool QByteArray::endsWith(char ch) const
{
   if (d->size == 0) {
      return false;
   }
   return d->data()[d->size - 1] == ch;
}

/*!
    Returns a byte array that contains the leftmost \a len bytes of
    this byte array.

    The entire byte array is returned if \a len is greater than
    size().

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 27

    \sa right(), mid(), startsWith(), truncate()
*/

QByteArray QByteArray::left(int len)  const
{
   if (len >= d->size) {
      return *this;
   }
   if (len < 0) {
      len = 0;
   }
   return QByteArray(d->data(), len);
}

/*!
    Returns a byte array that contains the rightmost \a len bytes of
    this byte array.

    The entire byte array is returned if \a len is greater than
    size().

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 28

    \sa endsWith(), left(), mid()
*/

QByteArray QByteArray::right(int len) const
{
   if (len >= d->size) {
      return *this;
   }
   if (len < 0) {
      len = 0;
   }
   return QByteArray(d->data() + d->size - len, len);
}

/*!
    Returns a byte array containing \a len bytes from this byte array,
    starting at position \a pos.

    If \a len is -1 (the default), or \a pos + \a len >= size(),
    returns a byte array containing all bytes starting at position \a
    pos until the end of the byte array.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 29

    \sa left(), right()
*/

QByteArray QByteArray::mid(int pos, int len) const
{
   if ((d->size == 0 && d->ref.isStatic()) || pos > d->size) {
      return QByteArray();
   }
   if (len < 0) {
      len = d->size - pos;
   }
   if (pos < 0) {
      len += pos;
      pos = 0;
   }
   if (len + pos > d->size) {
      len = d->size - pos;
   }
   if (pos == 0 && len == d->size) {
      return *this;
   }
   return QByteArray(d->data() + pos, len);
}

/*!
    Returns a lowercase copy of the byte array. The bytearray is
    interpreted as a Latin-1 encoded string.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 30

    \sa toUpper(), {8-bit Character Comparisons}
*/
QByteArray QByteArray::toLower() const
{
   QByteArray s(*this);
   uchar *p = reinterpret_cast<uchar *>(s.data());
   if (p) {
      while (*p) {
         *p = QChar::toLower((ushort) * p);
         p++;
      }
   }
   return s;
}

/*!
    Returns an uppercase copy of the byte array. The bytearray is
    interpreted as a Latin-1 encoded string.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 31

    \sa toLower(), {8-bit Character Comparisons}
*/

QByteArray QByteArray::toUpper() const
{
   QByteArray s(*this);
   uchar *p = reinterpret_cast<uchar *>(s.data());
   if (p) {
      while (*p) {
         *p = QChar::toUpper((ushort) * p);
         p++;
      }
   }
   return s;
}

/*! \fn void QByteArray::clear()

    Clears the contents of the byte array and makes it empty.

    \sa resize(), isEmpty()
*/

void QByteArray::clear()
{
   if (!d->ref.deref()) {
      Data::deallocate(d);
   }
   d = Data::sharedNull();
}

#if !defined(QT_NO_DATASTREAM)

/*! \relates QByteArray

    Writes byte array \a ba to the stream \a out and returns a reference
    to the stream.

    \sa {Serializing Qt Data Types}
*/

QDataStream &operator<<(QDataStream &out, const QByteArray &ba)
{
   if (ba.isNull() && out.version() >= 6) {
      out << (quint32)0xffffffff;
      return out;
   }
   return out.writeBytes(ba.constData(), ba.size());
}

/*! \relates QByteArray

    Reads a byte array into \a ba from the stream \a in and returns a
    reference to the stream.

    \sa {Serializing Qt Data Types}
*/

QDataStream &operator>>(QDataStream &in, QByteArray &ba)
{
   ba.clear();
   quint32 len;
   in >> len;
   if (len == 0xffffffff) {
      return in;
   }

   const quint32 Step = 1024 * 1024;
   quint32 allocated = 0;

   do {
      int blockSize = qMin(Step, len - allocated);
      ba.resize(allocated + blockSize);
      if (in.readRawData(ba.data() + allocated, blockSize) != blockSize) {
         ba.clear();
         in.setStatus(QDataStream::ReadPastEnd);
         return in;
      }
      allocated += blockSize;
   } while (allocated < len);

   return in;
}
#endif




QByteArray QByteArray::simplified() const
{
   if (d->size == 0) {
      return *this;
   }

   QByteArray result(d->size, Qt::Uninitialized);
   const char *from = d->data();
   const char *fromend = from + d->size;
   int outc = 0;
   char *to = result.d->data();

   for (;;) {
      while (from != fromend && isspace(uchar(*from))) {
         from++;
      }
      while (from != fromend && !isspace(uchar(*from))) {
         to[outc++] = *from++;
      }
      if (from != fromend) {
         to[outc++] = ' ';
      } else {
         break;
      }
   }

   if (outc > 0 && to[outc - 1] == ' ') {
      outc--;
   }

   result.resize(outc);

   return result;
}

/*!
    Returns a byte array that has whitespace removed from the start
    and the end.

    Whitespace means any character for which the standard C++
    isspace() function returns true. This includes the ASCII
    characters '\\t', '\\n', '\\v', '\\f', '\\r', and ' '.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 33

    Unlike simplified(), trimmed() leaves internal whitespace alone.

    \sa simplified()
*/
QByteArray QByteArray::trimmed() const
{
   if (d->size == 0) {
      return *this;
   }
   const char *s = d->data();
   if (!isspace(uchar(*s)) && !isspace(uchar(s[d->size - 1]))) {
      return *this;
   }
   int start = 0;
   int end = d->size - 1;
   while (start <= end && isspace(uchar(s[start]))) { // skip white space from start
      start++;
   }
   if (start <= end) {                          // only white space
      while (end && isspace(uchar(s[end]))) {         // skip white space from end
         end--;
      }
   }
   int l = end - start + 1;
   if (l <= 0) {
      QByteArrayDataPtr empty = { Data::allocate(0) };
      return QByteArray(empty);
   }
   return QByteArray(s + start, l);
}

/*!
    Returns a byte array of size \a width that contains this byte
    array padded by the \a fill character.

    If \a truncate is false and the size() of the byte array is more
    than \a width, then the returned byte array is a copy of this byte
    array.

    If \a truncate is true and the size() of the byte array is more
    than \a width, then any bytes in a copy of the byte array
    after position \a width are removed, and the copy is returned.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 34

    \sa rightJustified()
*/

QByteArray QByteArray::leftJustified(int width, char fill, bool truncate) const
{
   QByteArray result;
   int len = d->size;
   int padlen = width - len;
   if (padlen > 0) {
      result.resize(len + padlen);
      if (len) {
         memcpy(result.d->data(), d->data(), len);
      }
      memset(result.d->data() + len, fill, padlen);
   } else {
      if (truncate) {
         result = left(width);
      } else {
         result = *this;
      }
   }
   return result;
}

/*!
    Returns a byte array of size \a width that contains the \a fill
    character followed by this byte array.

    If \a truncate is false and the size of the byte array is more
    than \a width, then the returned byte array is a copy of this byte
    array.

    If \a truncate is true and the size of the byte array is more
    than \a width, then the resulting byte array is truncated at
    position \a width.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 35

    \sa leftJustified()
*/

QByteArray QByteArray::rightJustified(int width, char fill, bool truncate) const
{
   QByteArray result;
   int len = d->size;
   int padlen = width - len;

   if (padlen > 0) {
      result.resize(len + padlen);
      if (len) {
         memcpy(result.d->data() + padlen, data(), len);
      }
      memset(result.d->data(), fill, padlen);

   } else {
      if (truncate) {
         result = left(width);
      } else {
         result = *this;
      }
   }

   return result;
}

bool QByteArray::isNull() const
{
   return d == Data::sharedNull();
}


/*!
    Returns the byte array converted to a \c {long long} using base \a
    base, which is 10 by default and must be between 2 and 36, or 0.

    If \a base is 0, the base is determined automatically using the
    following rules: If the byte array begins with "0x", it is assumed to
    be hexadecimal; if it begins with "0", it is assumed to be octal;
    otherwise it is assumed to be decimal.

    Returns 0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/

qlonglong QByteArray::toLongLong(bool *ok, int base) const
{
#if defined(QT_CHECK_RANGE)
   if (base != 0 && (base < 2 || base > 36)) {
      qWarning("QByteArray::toLongLong: Invalid base %d", base);
      base = 10;
   }
#endif

   return QLocalePrivate::bytearrayToLongLong(nulTerminated().constData(), base, ok);
}

/*!
    Returns the byte array converted to an \c {unsigned long long}
    using base \a base, which is 10 by default and must be between 2
    and 36, or 0.

    If \a base is 0, the base is determined automatically using the
    following rules: If the byte array begins with "0x", it is assumed to
    be hexadecimal; if it begins with "0", it is assumed to be octal;
    otherwise it is assumed to be decimal.

    Returns 0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/

qulonglong QByteArray::toULongLong(bool *ok, int base) const
{
#if defined(QT_CHECK_RANGE)
   if (base != 0 && (base < 2 || base > 36)) {
      qWarning("QByteArray::toULongLong: Invalid base %d", base);
      base = 10;
   }
#endif

   return QLocalePrivate::bytearrayToUnsLongLong(nulTerminated().constData(), base, ok);
}


/*!
    Returns the byte array converted to an \c int using base \a
    base, which is 10 by default and must be between 2 and 36, or 0.

    If \a base is 0, the base is determined automatically using the
    following rules: If the byte array begins with "0x", it is assumed to
    be hexadecimal; if it begins with "0", it is assumed to be octal;
    otherwise it is assumed to be decimal.

    Returns 0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 36

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/

int QByteArray::toInt(bool *ok, int base) const
{
   qlonglong v = toLongLong(ok, base);
   if (v < INT_MIN || v > INT_MAX) {
      if (ok) {
         *ok = false;
      }
      v = 0;
   }
   return int(v);
}

/*!
    Returns the byte array converted to an \c {unsigned int} using base \a
    base, which is 10 by default and must be between 2 and 36, or 0.

    If \a base is 0, the base is determined automatically using the
    following rules: If the byte array begins with "0x", it is assumed to
    be hexadecimal; if it begins with "0", it is assumed to be octal;
    otherwise it is assumed to be decimal.

    Returns 0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/

uint QByteArray::toUInt(bool *ok, int base) const
{
   qulonglong v = toULongLong(ok, base);
   if (v > UINT_MAX) {
      if (ok) {
         *ok = false;
      }
      v = 0;
   }
   return uint(v);
}

/*!
    \since 4.1

    Returns the byte array converted to a \c long int using base \a
    base, which is 10 by default and must be between 2 and 36, or 0.

    If \a base is 0, the base is determined automatically using the
    following rules: If the byte array begins with "0x", it is assumed to
    be hexadecimal; if it begins with "0", it is assumed to be octal;
    otherwise it is assumed to be decimal.

    Returns 0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 37

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/
long QByteArray::toLong(bool *ok, int base) const
{
   qlonglong v = toLongLong(ok, base);
   if (v < LONG_MIN || v > LONG_MAX) {
      if (ok) {
         *ok = false;
      }
      v = 0;
   }
   return long(v);
}

/*!
    \since 4.1

    Returns the byte array converted to an \c {unsigned long int} using base \a
    base, which is 10 by default and must be between 2 and 36, or 0.

    If \a base is 0, the base is determined automatically using the
    following rules: If the byte array begins with "0x", it is assumed to
    be hexadecimal; if it begins with "0", it is assumed to be octal;
    otherwise it is assumed to be decimal.

    Returns 0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/
ulong QByteArray::toULong(bool *ok, int base) const
{
   qulonglong v = toULongLong(ok, base);
   if (v > ULONG_MAX) {
      if (ok) {
         *ok = false;
      }
      v = 0;
   }
   return ulong(v);
}

/*!
    Returns the byte array converted to a \c short using base \a
    base, which is 10 by default and must be between 2 and 36, or 0.

    If \a base is 0, the base is determined automatically using the
    following rules: If the byte array begins with "0x", it is assumed to
    be hexadecimal; if it begins with "0", it is assumed to be octal;
    otherwise it is assumed to be decimal.

    Returns 0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/

short QByteArray::toShort(bool *ok, int base) const
{
   qlonglong v = toLongLong(ok, base);
   if (v < SHRT_MIN || v > SHRT_MAX) {
      if (ok) {
         *ok = false;
      }
      v = 0;
   }
   return short(v);
}

/*!
    Returns the byte array converted to an \c {unsigned short} using base \a
    base, which is 10 by default and must be between 2 and 36, or 0.

    If \a base is 0, the base is determined automatically using the
    following rules: If the byte array begins with "0x", it is assumed to
    be hexadecimal; if it begins with "0", it is assumed to be octal;
    otherwise it is assumed to be decimal.

    Returns 0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/

ushort QByteArray::toUShort(bool *ok, int base) const
{
   qulonglong v = toULongLong(ok, base);
   if (v > USHRT_MAX) {
      if (ok) {
         *ok = false;
      }
      v = 0;
   }
   return ushort(v);
}


/*!
    Returns the byte array converted to a \c double value.

    Returns 0.0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 38

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/

double QByteArray::toDouble(bool *ok) const
{
   return QLocalePrivate::bytearrayToDouble(nulTerminated().constData(), ok);
}

/*!
    Returns the byte array converted to a \c float value.

    Returns 0.0 if the conversion fails.

    If \a ok is not 0: if a conversion error occurs, *\a{ok} is set to
    false; otherwise *\a{ok} is set to true.

    \note The conversion of the number is performed in the default C locale,
    irrespective of the user's locale.

    \sa number()
*/

float QByteArray::toFloat(bool *ok) const
{
   return float(toDouble(ok));
}

/*!
    Returns a copy of the byte array, encoded as Base64.

    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 39

    The algorithm used to encode Base64-encoded data is defined in \l{RFC 2045}.

    \sa fromBase64()
*/
QByteArray QByteArray::toBase64() const
{
   const char alphabet[] = "ABCDEFGH" "IJKLMNOP" "QRSTUVWX" "YZabcdef"
                           "ghijklmn" "opqrstuv" "wxyz0123" "456789+/";
   const char padchar = '=';
   int padlen = 0;

   QByteArray tmp((d->size * 4) / 3 + 3, Qt::Uninitialized);

   int i = 0;
   char *out = tmp.data();
   while (i < d->size) {
      int chunk = 0;
      chunk |= int(uchar(d->data()[i++])) << 16;
      if (i == d->size) {
         padlen = 2;
      } else {
         chunk |= int(uchar(d->data()[i++])) << 8;
         if (i == d->size) {
            padlen = 1;
         } else {
            chunk |= int(uchar(d->data()[i++]));
         }
      }

      int j = (chunk & 0x00fc0000) >> 18;
      int k = (chunk & 0x0003f000) >> 12;
      int l = (chunk & 0x00000fc0) >> 6;
      int m = (chunk & 0x0000003f);
      *out++ = alphabet[j];
      *out++ = alphabet[k];
      if (padlen > 1) {
         *out++ = padchar;
      } else {
         *out++ = alphabet[l];
      }
      if (padlen > 0) {
         *out++ = padchar;
      } else {
         *out++ = alphabet[m];
      }
   }

   tmp.truncate(out - tmp.data());
   return tmp;
}

/*!
    \fn QByteArray &QByteArray::setNum(int n, int base)

    Sets the byte array to the printed value of \a n in base \a base (10
    by default) and returns a reference to the byte array. The \a base can
    be any value between 2 and 36. For bases other than 10, n is treated
    as an unsigned integer.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 40

    \note The format of the number is not localized; the default C locale
    is used irrespective of the user's locale.

    \sa number(), toInt()
*/

/*!
    \fn QByteArray &QByteArray::setNum(uint n, int base)
    \overload

    \sa toUInt()
*/

/*!
    \fn QByteArray &QByteArray::setNum(short n, int base)
    \overload

    \sa toShort()
*/

/*!
    \fn QByteArray &QByteArray::setNum(ushort n, int base)
    \overload

    \sa toUShort()
*/

/*!
    \overload

    \sa toLongLong()
*/

static char *qulltoa2(char *p, qulonglong n, int base)
{
#if defined(QT_CHECK_RANGE)
   if (base < 2 || base > 36) {
      qWarning("QByteArray::setNum: Invalid base %d", base);
      base = 10;
   }
#endif
   const char b = 'a' - 10;
   do {
      const int c = n % base;
      n /= base;
      *--p = c + (c < 10 ? '0' : b);
   } while (n);

   return p;
}

QByteArray &QByteArray::setNum(qlonglong n, int base)
{
   const int buffsize = 66; // big enough for MAX_ULLONG in base 2
   char buff[buffsize];
   char *p;

   if (n < 0 && base == 10) {
      p = qulltoa2(buff + buffsize, qulonglong(-(1 + n)) + 1, base);
      *--p = '-';
   } else {
      p = qulltoa2(buff + buffsize, qulonglong(n), base);
   }

   clear();
   append(p, buffsize - (p - buff));
   return *this;
}

/*!
    \overload

    \sa toULongLong()
*/

QByteArray &QByteArray::setNum(qulonglong n, int base)
{
   const int buffsize = 66; // big enough for MAX_ULLONG in base 2
   char buff[buffsize];
   char *p = qulltoa2(buff + buffsize, n, base);

   clear();
   append(p, buffsize - (p - buff));
   return *this;
}

/*!
    \overload

    Sets the byte array to the printed value of \a n, formatted in format
    \a f with precision \a prec, and returns a reference to the
    byte array.

    The format \a f can be any of the following:

    \table
    \header \i Format \i Meaning
    \row \i \c e \i format as [-]9.9e[+|-]999
    \row \i \c E \i format as [-]9.9E[+|-]999
    \row \i \c f \i format as [-]9.9
    \row \i \c g \i use \c e or \c f format, whichever is the most concise
    \row \i \c G \i use \c E or \c f format, whichever is the most concise
    \endtable

    With 'e', 'E', and 'f', \a prec is the number of digits after the
    decimal point. With 'g' and 'G', \a prec is the maximum number of
    significant digits (trailing zeroes are omitted).

    \note The format of the number is not localized; the default C locale
    is used irrespective of the user's locale.

    \sa toDouble()
*/

QByteArray &QByteArray::setNum(double n, char f, int prec)
{
   QLocalePrivate::DoubleForm form = QLocalePrivate::DFDecimal;
   uint flags = 0;

   if (qIsUpper(f)) {
      flags = QLocalePrivate::CapitalEorX;
   }
   f = qToLower(f);

   switch (f) {
      case 'f':
         form = QLocalePrivate::DFDecimal;
         break;
      case 'e':
         form = QLocalePrivate::DFExponent;
         break;
      case 'g':
         form = QLocalePrivate::DFSignificantDigits;
         break;
      default:
#if defined(QT_CHECK_RANGE)
         qWarning("QByteArray::setNum: Invalid format char '%c'", f);
#endif
         break;
   }

   QLocale locale(QLocale::C);
   *this = locale.d()->doubleToString(n, prec, form, -1, flags).toLatin1();
   return *this;
}

/*!
    \fn QByteArray &QByteArray::setNum(float n, char f, int prec)
    \overload

    Sets the byte array to the printed value of \a n, formatted in format
    \a f with precision \a prec, and returns a reference to the
    byte array.

    \note The format of the number is not localized; the default C locale
    is used irrespective of the user's locale.

    \sa toFloat()
*/

/*!
    Returns a byte array containing the string equivalent of the
    number \a n to base \a base (10 by default). The \a base can be
    any value between 2 and 36.

    Example:
    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 41

    \note The format of the number is not localized; the default C locale
    is used irrespective of the user's locale.

    \sa setNum(), toInt()
*/
QByteArray QByteArray::number(int n, int base)
{
   QByteArray s;
   s.setNum(n, base);
   return s;
}

/*!
    \overload

    \sa toUInt()
*/
QByteArray QByteArray::number(uint n, int base)
{
   QByteArray s;
   s.setNum(n, base);
   return s;
}

/*!
    \overload

    \sa toLongLong()
*/
QByteArray QByteArray::number(qlonglong n, int base)
{
   QByteArray s;
   s.setNum(n, base);
   return s;
}

/*!
    \overload

    \sa toULongLong()
*/
QByteArray QByteArray::number(qulonglong n, int base)
{
   QByteArray s;
   s.setNum(n, base);
   return s;
}

/*!
    \overload

    Returns a byte array that contains the printed value of \a n,
    formatted in format \a f with precision \a prec.

    Argument \a n is formatted according to the \a f format specified,
    which is \c g by default, and can be any of the following:

    \table
    \header \i Format \i Meaning
    \row \i \c e \i format as [-]9.9e[+|-]999
    \row \i \c E \i format as [-]9.9E[+|-]999
    \row \i \c f \i format as [-]9.9
    \row \i \c g \i use \c e or \c f format, whichever is the most concise
    \row \i \c G \i use \c E or \c f format, whichever is the most concise
    \endtable

    With 'e', 'E', and 'f', \a prec is the number of digits after the
    decimal point. With 'g' and 'G', \a prec is the maximum number of
    significant digits (trailing zeroes are omitted).

    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 42

    \note The format of the number is not localized; the default C locale
    is used irrespective of the user's locale.

    \sa toDouble()
*/
QByteArray QByteArray::number(double n, char f, int prec)
{
   QByteArray s;
   s.setNum(n, f, prec);
   return s;
}

/*!
    Constructs a QByteArray that uses the first \a size bytes of the
    \a data array. The bytes are \e not copied. The QByteArray will
    contain the \a data pointer. The caller guarantees that \a data
    will not be deleted or modified as long as this QByteArray and any
    copies of it exist that have not been modified. In other words,
    because QByteArray is an \l{implicitly shared} class and the
    instance returned by this function contains the \a data pointer,
    the caller must not delete \a data or modify it directly as long
    as the returned QByteArray and any copies exist. However,
    QByteArray does not take ownership of \a data, so the QByteArray
    destructor will never delete the raw \a data, even when the
    last QByteArray referring to \a data is destroyed.

    A subsequent attempt to modify the contents of the returned
    QByteArray or any copy made from it will cause it to create a deep
    copy of the \a data array before doing the modification. This
    ensures that the raw \a data array itself will never be modified
    by QByteArray.

    Here is an example of how to read data using a QDataStream on raw
    data in memory without copying the raw data into a QByteArray:

    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 43

    \warning A byte array created with fromRawData() is \e not
    null-terminated, unless the raw data contains a 0 character at
    position \a size. While that does not matter for QDataStream or
    functions like indexOf(), passing the byte array to a function
    accepting a \c{const char *} expected to be '\\0'-terminated will
    fail.

    \sa setRawData(), data(), constData()
*/

QByteArray QByteArray::fromRawData(const char *data, int size)
{
   Data *x;
   if (!data) {
      x = Data::sharedNull();
   } else if (!size) {
      x = Data::allocate(0);
   } else {
      x = Data::fromRawData(data, size);
      Q_CHECK_PTR(x);
   }
   QByteArrayDataPtr dataPtr = { x };
   return QByteArray(dataPtr);
}

/*!
    \since 4.7

    Resets the QByteArray to use the first \a size bytes of the
    \a data array. The bytes are \e not copied. The QByteArray will
    contain the \a data pointer. The caller guarantees that \a data
    will not be deleted or modified as long as this QByteArray and any
    copies of it exist that have not been modified.

    This function can be used instead of fromRawData() to re-use
    existings QByteArray objects to save memory re-allocations.

    \sa fromRawData(), data(), constData()
*/
QByteArray &QByteArray::setRawData(const char *data, uint size)
{
   if (d->ref.isShared() || d->alloc) {
      *this = fromRawData(data, size);
   } else {
      if (data) {
         d->size = size;
         d->offset = data - reinterpret_cast<char *>(d);
      } else {
         d->offset = sizeof(QByteArrayData);
         d->size = 0;
         *d->data() = 0;
      }
   }
   return *this;
}

/*!
    Returns a decoded copy of the Base64 array \a base64. Input is not checked
    for validity; invalid characters in the input are skipped, enabling the
    decoding process to continue with subsequent characters.

    For example:

    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 44

    The algorithm used to decode Base64-encoded data is defined in \l{RFC 2045}.

    \sa toBase64()
*/
QByteArray QByteArray::fromBase64(const QByteArray &base64)
{
   unsigned int buf = 0;
   int nbits = 0;
   QByteArray tmp((base64.size() * 3) / 4, Qt::Uninitialized);

   int offset = 0;
   for (int i = 0; i < base64.size(); ++i) {
      int ch = base64.at(i);
      int d;

      if (ch >= 'A' && ch <= 'Z') {
         d = ch - 'A';
      } else if (ch >= 'a' && ch <= 'z') {
         d = ch - 'a' + 26;
      } else if (ch >= '0' && ch <= '9') {
         d = ch - '0' + 52;
      } else if (ch == '+') {
         d = 62;
      } else if (ch == '/') {
         d = 63;
      } else {
         d = -1;
      }

      if (d != -1) {
         buf = (buf << 6) | d;
         nbits += 6;
         if (nbits >= 8) {
            nbits -= 8;
            tmp[offset++] = buf >> nbits;
            buf &= (1 << nbits) - 1;
         }
      }
   }

   tmp.truncate(offset);
   return tmp;
}

/*!
    Returns a decoded copy of the hex encoded array \a hexEncoded. Input is not checked
    for validity; invalid characters in the input are skipped, enabling the
    decoding process to continue with subsequent characters.

    For example:

    \snippet doc/src/snippets/code/src_corelib_tools_qbytearray.cpp 45

    \sa toHex()
*/
QByteArray QByteArray::fromHex(const QByteArray &hexEncoded)
{
   QByteArray res((hexEncoded.size() + 1) / 2, Qt::Uninitialized);
   uchar *result = (uchar *)res.data() + res.size();

   bool odd_digit = true;
   for (int i = hexEncoded.size() - 1; i >= 0; --i) {
      int ch = hexEncoded.at(i);
      int tmp;
      if (ch >= '0' && ch <= '9') {
         tmp = ch - '0';
      } else if (ch >= 'a' && ch <= 'f') {
         tmp = ch - 'a' + 10;
      } else if (ch >= 'A' && ch <= 'F') {
         tmp = ch - 'A' + 10;
      } else {
         continue;
      }
      if (odd_digit) {
         --result;
         *result = tmp;
         odd_digit = false;
      } else {
         *result |= tmp << 4;
         odd_digit = true;
      }
   }

   res.remove(0, result - (const uchar *)res.constData());
   return res;
}

/*!
    Returns a hex encoded copy of the byte array. The hex encoding uses the numbers 0-9 and
    the letters a-f.

    \sa fromHex()
*/
QByteArray QByteArray::toHex() const
{
   QByteArray hex(d->size * 2, Qt::Uninitialized);
   char *hexData = hex.data();
   const uchar *data = (const uchar *)d->data();
   for (int i = 0; i < d->size; ++i) {
      int j = (data[i] >> 4) & 0xf;
      if (j <= 9) {
         hexData[i * 2] = (j + '0');
      } else {
         hexData[i * 2] = (j + 'a' - 10);
      }
      j = data[i] & 0xf;
      if (j <= 9) {
         hexData[i * 2 + 1] = (j + '0');
      } else {
         hexData[i * 2 + 1] = (j + 'a' - 10);
      }
   }
   return hex;
}

static void q_fromPercentEncoding(QByteArray *ba, char percent)
{
   if (ba->isEmpty()) {
      return;
   }

   char *data = ba->data();
   const char *inputPtr = data;

   int i = 0;
   int len = ba->count();
   int outlen = 0;
   int a, b;
   char c;
   while (i < len) {
      c = inputPtr[i];
      if (c == percent && i + 2 < len) {
         a = inputPtr[++i];
         b = inputPtr[++i];

         if (a >= '0' && a <= '9') {
            a -= '0';
         } else if (a >= 'a' && a <= 'f') {
            a = a - 'a' + 10;
         } else if (a >= 'A' && a <= 'F') {
            a = a - 'A' + 10;
         }

         if (b >= '0' && b <= '9') {
            b -= '0';
         } else if (b >= 'a' && b <= 'f') {
            b  = b - 'a' + 10;
         } else if (b >= 'A' && b <= 'F') {
            b  = b - 'A' + 10;
         }

         *data++ = (char)((a << 4) | b);
      } else {
         *data++ = c;
      }

      ++i;
      ++outlen;
   }

   if (outlen != len) {
      ba->truncate(outlen);
   }
}

void q_fromPercentEncoding(QByteArray *ba)
{
   q_fromPercentEncoding(ba, '%');
}

/*!
    \since 4.4

    Returns a decoded copy of the URI/URL-style percent-encoded \a input.
    The \a percent parameter allows you to replace the '%' character for
    another (for instance, '_' or '=').

    For example:
    \code
        QByteArray text = QByteArray::fromPercentEncoding("Qt%20is%20great%33");
        text.data();            // returns "Qt is great!"
    \endcode

    \sa toPercentEncoding(), QUrl::fromPercentEncoding()
*/
QByteArray QByteArray::fromPercentEncoding(const QByteArray &input, char percent)
{
   if (input.isNull()) {
      return QByteArray();   // preserve null
   }
   if (input.isEmpty()) {
      return QByteArray(input.data(), 0);
   }

   QByteArray tmp = input;
   q_fromPercentEncoding(&tmp, percent);
   return tmp;
}

static inline bool q_strchr(const char str[], char chr)
{
   if (!str) {
      return false;
   }

   const char *ptr = str;
   char c;
   while ((c = *ptr++))
      if (c == chr) {
         return true;
      }
   return false;
}

static inline char toHexHelper(char c)
{
   static const char hexnumbers[] = "0123456789ABCDEF";
   return hexnumbers[c & 0xf];
}

static void q_toPercentEncoding(QByteArray *ba, const char *dontEncode, const char *alsoEncode, char percent)
{
   if (ba->isEmpty()) {
      return;
   }

   QByteArray input = *ba;
   int len = input.count();
   const char *inputData = input.constData();
   char *output = 0;
   int length = 0;

   for (int i = 0; i < len; ++i) {
      unsigned char c = *inputData++;
      if (((c >= 0x61 && c <= 0x7A) // ALPHA
            || (c >= 0x41 && c <= 0x5A) // ALPHA
            || (c >= 0x30 && c <= 0x39) // DIGIT
            || c == 0x2D // -
            || c == 0x2E // .
            || c == 0x5F // _
            || c == 0x7E // ~
            || q_strchr(dontEncode, c))
            && !q_strchr(alsoEncode, c)) {
         if (output) {
            output[length] = c;
         }
         ++length;
      } else {
         if (!output) {
            // detach now
            ba->resize(len * 3); // worst case
            output = ba->data();
         }
         output[length++] = percent;
         output[length++] = toHexHelper((c & 0xf0) >> 4);
         output[length++] = toHexHelper(c & 0xf);
      }
   }
   if (output) {
      ba->truncate(length);
   }
}

void q_toPercentEncoding(QByteArray *ba, const char *exclude, const char *include)
{
   q_toPercentEncoding(ba, exclude, include, '%');
}

void q_normalizePercentEncoding(QByteArray *ba, const char *exclude)
{
   q_fromPercentEncoding(ba, '%');
   q_toPercentEncoding(ba, exclude, 0, '%');
}

/*!
    \since 4.4

    Returns a URI/URL-style percent-encoded copy of this byte array. The
    \a percent parameter allows you to override the default '%'
    character for another.

    By default, this function will encode all characters that are not
    one of the following:

        ALPHA ("a" to "z" and "A" to "Z") / DIGIT (0 to 9) / "-" / "." / "_" / "~"

    To prevent characters from being encoded pass them to \a
    exclude. To force characters to be encoded pass them to \a
    include. The \a percent character is always encoded.

    Example:

    \code
         QByteArray text = "{a fishy string?}";
         QByteArray ba = text.toPercentEncoding("{}", "s");
         qDebug(ba.constData());
         // prints "{a fi%73hy %73tring%3F}"
    \endcode

    The hex encoding uses the numbers 0-9 and the uppercase letters A-F.

    \sa fromPercentEncoding(), QUrl::toPercentEncoding()
*/
QByteArray QByteArray::toPercentEncoding(const QByteArray &exclude, const QByteArray &include,
      char percent) const
{
   if (isNull()) {
      return QByteArray();   // preserve null
   }
   if (isEmpty()) {
      return QByteArray(data(), 0);
   }

   QByteArray include2 = include;
   if (percent != '%')                        // the default
      if ((percent >= 0x61 && percent <= 0x7A) // ALPHA
            || (percent >= 0x41 && percent <= 0x5A) // ALPHA
            || (percent >= 0x30 && percent <= 0x39) // DIGIT
            || percent == 0x2D // -
            || percent == 0x2E // .
            || percent == 0x5F // _
            || percent == 0x7E) { // ~
         include2 += percent;
      }

   QByteArray result = *this;
   q_toPercentEncoding(&result, exclude.nulTerminated().constData(), include2.nulTerminated().constData(), percent);

   return result;
}

/*! \typedef QByteArray::ConstIterator
    \internal
*/

/*! \typedef QByteArray::Iterator
    \internal
*/

/*! \typedef QByteArray::const_iterator
    \internal
*/

/*! \typedef QByteArray::iterator
    \internal
*/

/*! \typedef QByteArray::const_reference
    \internal
*/

/*! \typedef QByteArray::reference
    \internal
*/

/*! \typedef QByteArray::value_type
  \internal
 */

/*!
    \fn QByteArray::QByteArray(int size)

    Use QByteArray(int, char) instead.
*/


/*!
    \fn QByteArray QByteArray::leftJustify(uint width, char fill, bool truncate) const

    Use leftJustified() instead.
*/

/*!
    \fn QByteArray QByteArray::rightJustify(uint width, char fill, bool truncate) const

    Use rightJustified() instead.
*/

/*!
    \fn QByteArray& QByteArray::duplicate(const QByteArray& a)

    \oldcode
        QByteArray bdata;
        bdata.duplicate(original);
    \newcode
        QByteArray bdata;
        bdata = original;
    \endcode

    \note QByteArray uses implicit sharing so if you modify a copy, only the
    copy is changed.
*/

/*!
    \fn QByteArray& QByteArray::duplicate(const char *a, uint n)

    \overload

    \oldcode
        QByteArray bdata;
        bdata.duplicate(ptr, size);
    \newcode
        QByteArray bdata;
        bdata = QByteArray(ptr, size);
    \endcode

    \note QByteArray uses implicit sharing so if you modify a copy, only the
    copy is changed.
*/

/*!
    \fn void QByteArray::resetRawData(const char *data, uint n)

    Use clear() instead.
*/

/*!
    \fn QByteArray QByteArray::lower() const

    Use toLower() instead.
*/

/*!
    \fn QByteArray QByteArray::upper() const

    Use toUpper() instead.
*/

/*!
    \fn QByteArray QByteArray::stripWhiteSpace() const

    Use trimmed() instead.
*/

/*!
    \fn QByteArray QByteArray::simplifyWhiteSpace() const

    Use simplified() instead.
*/

/*!
    \fn int QByteArray::find(char c, int from = 0) const

    Use indexOf() instead.
*/

/*!
    \fn int QByteArray::find(const char *c, int from = 0) const

    Use indexOf() instead.
*/

/*!
    \fn int QByteArray::find(const QByteArray &ba, int from = 0) const

    Use indexOf() instead.
*/

/*!
    \fn int QByteArray::findRev(char c, int from = -1) const

    Use lastIndexOf() instead.
*/

/*!
    \fn int QByteArray::findRev(const char *c, int from = -1) const

    Use lastIndexOf() instead.
*/

/*!
    \fn int QByteArray::findRev(const QByteArray &ba, int from = -1) const

    Use lastIndexOf() instead.
*/

/*!
    \fn int QByteArray::find(const QString &s, int from = 0) const

    Use indexOf() instead.
*/

/*!
    \fn int QByteArray::findRev(const QString &s, int from = -1) const

    Use lastIndexOf() instead.
*/

/*!
    \fn DataPtr &QByteArray::data_ptr()
    \internal
*/

/*!
    \typedef QByteArray::DataPtr
    \internal
*/

QT_END_NAMESPACE
