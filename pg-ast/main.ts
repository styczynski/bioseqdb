import { parse, deparse } from 'pgsql-parser';
import util from 'util';

// Parse SQL to AST
const stmts = await parse('create table x ( a INT, seq TEXT ) using columnar;');

console.log(util.inspect(stmts, {depth: null}));
