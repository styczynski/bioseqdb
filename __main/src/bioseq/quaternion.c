/* Copyright 2020 Mats Kindahl
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. */

/*
 * This file introduce the quarternion type.
 *
 * It is based on the example un "User-Defined Types" at
 * https://www.postgresql.org/docs/10/xtypes.html
 */

#include "quaternion.h"

#include <postgres.h>
#include <fmgr.h>

#include <lib/stringinfo.h>
#include <libpq/pqformat.h>

#include <string.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(quaternion_in);

Datum quaternion_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  double a, b, c, d;
  Quaternion *result;

  if (sscanf(str, " ( %lf , %lf , %lf , %lf )", &a, &b, &c, &d) != 4)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for quaternion: \"%s\"", str)));

  result = (Quaternion *)palloc(sizeof(Quaternion));
  result->a = a;
  result->b = b;
  result->c = c;
  result->d = d;
  PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(quaternion_out);

Datum quaternion_out(PG_FUNCTION_ARGS) {
  Quaternion *quat = (Quaternion *)PG_GETARG_POINTER(0);
  char *result = psprintf("(%g,%g,%g,%g)", quat->a, quat->b, quat->c, quat->d);
  PG_RETURN_CSTRING(result);
}

PG_FUNCTION_INFO_V1(quaternion_recv);

Datum quaternion_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  Quaternion *result;

  result = (Quaternion *)palloc(sizeof(Quaternion));
  result->a = pq_getmsgfloat8(buf);
  result->b = pq_getmsgfloat8(buf);
  result->c = pq_getmsgfloat8(buf);
  result->d = pq_getmsgfloat8(buf);
  PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(quaternion_send);

Datum quaternion_send(PG_FUNCTION_ARGS) {
  Quaternion *quat = (Quaternion *)PG_GETARG_POINTER(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendfloat8(&buf, quat->a);
  pq_sendfloat8(&buf, quat->b);
  pq_sendfloat8(&buf, quat->c);
  pq_sendfloat8(&buf, quat->d);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}
