drop extension pgtam cascade;
create extension pgtam;
create table x(a int) using mem;
create table y(a int) using mem;
INSERT INTO x VALUES (23), (101);
select a from x;
select a from x where a = 23;
select a from y;
