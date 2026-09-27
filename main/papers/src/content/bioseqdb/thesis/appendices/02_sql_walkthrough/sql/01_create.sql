\set ON_ERROR_STOP on
\pset pager off
CREATE EXTENSION bioseq;

CREATE TABLE samples (
  id integer PRIMARY KEY,
  upload_date date NOT NULL,
  patient_name text NOT NULL,
  age integer NOT NULL,
  seq bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
