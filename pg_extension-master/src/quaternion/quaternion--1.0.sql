-- Copyright 2020 Mats Kindahl
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

CREATE TYPE quaternion;

CREATE FUNCTION quaternion_in(cstring)
    RETURNS quaternion
    AS 'MODULE_PATHNAME', 'quaternion_in'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION quaternion_out(quaternion)
    RETURNS cstring
    AS 'MODULE_PATHNAME', 'quaternion_out'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION quaternion_send(quaternion)
    RETURNS bytea
    AS 'MODULE_PATHNAME', 'quaternion_send'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION quaternion_recv(internal)
    RETURNS quaternion
    AS 'MODULE_PATHNAME', 'quaternion_recv'
    LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE quaternion (
   internallength = 32,
   input = quaternion_in,
   output = quaternion_out,
   receive = quaternion_recv,
   send = quaternion_send,
   alignment = double
);
