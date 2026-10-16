// Pure selection logic shared by the blog controls and their contract tests.
export const BLOG_CATEGORY_KEY = 'bioseq-blog-category';
export const BLOG_CATEGORIES = ['bioseqdb', 'others'];
export const BLOG_SORTS = ['newest', 'oldest', 'title'];

export function blogCategory(value) {
  return BLOG_CATEGORIES.includes(value) ? value : 'bioseqdb';
}

export function normalizeBlogText(value) {
  return String(value || '').normalize('NFKD').replace(/\p{M}/gu, '').toLowerCase().replace(/\s+/g, ' ').trim();
}

const compare = (left, right) => left < right ? -1 : left > right ? 1 : 0;

export function selectBlogPosts(posts, state, pageSize = 6) {
  const category = blogCategory(state.category);
  const terms = normalizeBlogText(state.query).split(' ').filter(Boolean);
  const keyword = normalizeBlogText(state.keyword);
  const selected = posts.filter(post => post.category === category
    && (!keyword || post.keywords.some(value => normalizeBlogText(value) === keyword))
    && terms.every(term => post.search.includes(term)));
  const byTitle = (left, right) => compare(normalizeBlogText(left.title), normalizeBlogText(right.title))
    || compare(left.id, right.id);
  selected.sort((left, right) => state.sort === 'title' ? byTitle(left, right)
    : (state.sort === 'oldest' ? compare(left.date, right.date) : compare(right.date, left.date))
      || byTitle(left, right));
  const size = Number.isSafeInteger(pageSize) && pageSize > 0 ? pageSize : 6;
  const pages = Math.max(1, Math.ceil(selected.length / size));
  const requested = Number(state.page);
  const page = Math.max(1, Math.min(pages, Number.isSafeInteger(requested) ? requested : 1));
  return { posts: selected.slice((page - 1) * size, page * size), count: selected.length, page, pages };
}
