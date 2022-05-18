
CREATE TYPE sex AS ENUM ('Female', 'Male');

CREATE TABLE dataset (
    id serial primary key not null,
    strain varchar(60) not null,
    seq nuclseq not null,
    retrieval_date date,
    submitted_date date,
    region varchar (60),
	country varchar (60),
    division varchar (60),
    location varchar (60),
    age interval,
    sex sex,
    lineage varchar(60)
);

