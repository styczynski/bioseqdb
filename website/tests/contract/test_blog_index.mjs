import assert from 'node:assert/strict';
import test from 'node:test';
import { blogCategory, normalizeBlogText, selectBlogPosts } from '../../static/js/blog-index-state.mjs';

const makePost = (id, category, title, date, keywords, body = '') => ({
  id, category, title, date, keywords,
  search: normalizeBlogText(`${title} ${keywords.join(' ')} ${body}`),
});
const posts = [
  makePost('bioseq', 'bioseqdb', 'Introducing BioseqDB', '2026-10-01', ['SQL', 'sequence alignment']),
  makePost('vectors', 'others', 'Vector search', '2026-09-30', ['SQL', 'search'], 'Genomic samples in a database'),
  makePost('tables', 'others', 'Table access methods', '2026-10-01', ['storage'], 'Heap pages and tuple slots'),
  makePost('encryption', 'others', 'Encrypted tables', '2026-10-01', ['storage', 'security'], 'Protect stored patient records'),
];
const state = { category: 'others', query: '', keyword: '', sort: 'newest', page: 1 };

test('BioseqDB is the default category, including invalid saved values', () => {
  assert.equal(blogCategory(undefined), 'bioseqdb');
  assert.equal(blogCategory('old-category'), 'bioseqdb');
  assert.equal(blogCategory('others'), 'others');
  assert.deepEqual(selectBlogPosts(posts, {}).posts.map(post => post.id), ['bioseq']);
});

test('blog-only full text search combines terms and exact keyword filters within the category', () => {
  assert.deepEqual(selectBlogPosts(posts, { ...state, query: 'DATABASE samples', keyword: 'sql' }).posts.map(p => p.id), ['vectors']);
  assert.equal(selectBlogPosts(posts, { ...state, query: 'BioseqDB' }).count, 0);
  assert.equal(selectBlogPosts(posts, { ...state, query: 'patient', keyword: 'search' }).count, 0);
  assert.equal(selectBlogPosts(posts, { ...state, keyword: 'stor' }).count, 0);
  assert.equal(normalizeBlogText('  Piotr Styczyński\n '), 'piotr styczynski');
});

test('sorts dates and titles deterministically before pagination', () => {
  assert.deepEqual(selectBlogPosts(posts, state).posts.map(p => p.id), ['encryption', 'tables', 'vectors']);
  assert.deepEqual(selectBlogPosts(posts, { ...state, sort: 'oldest' }).posts.map(p => p.id), ['vectors', 'encryption', 'tables']);
  assert.deepEqual(selectBlogPosts([...posts].reverse(), { ...state, sort: 'title' }).posts.map(p => p.id), ['encryption', 'tables', 'vectors']);
  assert.deepEqual(selectBlogPosts(posts, { ...state, sort: 'oldest', page: 2 }, 2).posts.map(p => p.id), ['tables']);
  assert.deepEqual(posts.map(p => p.id), ['bioseq', 'vectors', 'tables', 'encryption']);
});

test('paging clamps stale or malformed input after filtering and handles empty results', () => {
  const large = Array.from({ length: 14 }, (_, index) => makePost(String(index), 'others', `Entry ${String(index).padStart(2, '0')}`, '2026-10-01', ['SQL']));
  const last = selectBlogPosts(large, { ...state, page: 99 });
  assert.equal(last.page, 3);
  assert.equal(last.pages, 3);
  assert.equal(last.count, 14);
  assert.deepEqual(last.posts.map(p => p.id), ['12', '13']);
  for (const page of [0, -4, 'bad', 1.2, Infinity]) {
    assert.equal(selectBlogPosts(large, { ...state, page }).page, 1);
  }
  const empty = selectBlogPosts(large, { ...state, query: 'absent', page: 3 });
  assert.equal(empty.page, 1);
  assert.equal(empty.pages, 1);
  assert.equal(empty.count, 0);
  assert.deepEqual(empty.posts, []);
});
