import { BLOG_CATEGORY_KEY, BLOG_SORTS, blogCategory, normalizeBlogText, selectBlogPosts } from './blog-index-state.mjs';

const root = document.querySelector('[data-blog-browser]');
if (root) {
  const list = root.querySelector('[data-blog-posts]');
  const search = root.querySelector('[data-blog-search]');
  const keyword = root.querySelector('[data-blog-keyword-select]');
  const sort = root.querySelector('[data-blog-sort]');
  const count = root.querySelector('[data-blog-result-count]');
  const previous = root.querySelector('[data-blog-previous]');
  const next = root.querySelector('[data-blog-next]');
  const reset = root.querySelector('[data-blog-reset]');
  const categories = [...root.querySelectorAll('[data-blog-category-button]')];
  const cards = [...root.querySelectorAll('[data-blog-card]')].map((node, index) => {
    const title = node.dataset.blogTitle || '';
    const keywords = [...node.querySelectorAll('[data-blog-keyword]')].map(item => item.textContent.trim());
    return { node, id: String(index), title, keywords, category: blogCategory(node.dataset.blogCategory),
      date: node.dataset.blogDate || '',
      search: normalizeBlogText(`${title} ${keywords.join(' ')} ${node.textContent} ${node.dataset.blogSearchText || ''}`) };
  });
  const allKeywords = [...new Set(cards.flatMap(post => post.keywords))].sort((a, b) => a.localeCompare(b, 'en'));
  for (const value of allKeywords) {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = value;
    keyword.append(option);
  }
  const readCategory = () => {
    try { return blogCategory(localStorage.getItem(BLOG_CATEGORY_KEY)); }
    catch { return 'bioseqdb'; }
  };
  const readState = () => {
    const params = new URL(location.href).searchParams;
    return { category: params.has('category') ? blogCategory(params.get('category')) : readCategory(),
      query: params.get('q') || '', keyword: allKeywords.includes(params.get('keyword')) ? params.get('keyword') : '',
      sort: BLOG_SORTS.includes(params.get('sort')) ? params.get('sort') : 'newest', page: params.get('page') || 1 };
  };
  let state = readState();
  const saveCategory = () => {
    try { localStorage.setItem(BLOG_CATEGORY_KEY, state.category); }
    catch { /* Filtering still works when browser storage is unavailable. */ }
  };
  const format = (template, values) => String(template).replace(/\{(\w+)\}/g, (match, key) => values[key] ?? match);
  const writeUrl = () => {
    const url = new URL(location.href);
    // Keep the selected category explicit so a copied URL means the same thing
    // in another browser, regardless of that reader's saved preference.
    for (const [key, value] of Object.entries({ category: state.category, q: state.query,
      keyword: state.keyword, sort: state.sort === 'newest' ? '' : state.sort, page: state.page > 1 ? state.page : '' })) {
      if (value) url.searchParams.set(key, String(value));
      else url.searchParams.delete(key);
    }
    history.replaceState(null, '', url);
  };
  const render = (write = true) => {
    const result = selectBlogPosts(cards, state, Number(root.dataset.pageSize));
    state.page = result.page;
    root.dataset.blogCount = String(result.count);
    const visible = new Set(result.posts);
    for (const card of cards) card.node.hidden = !visible.has(card);
    // Move existing semantic articles; links and the no-JavaScript content
    // stay intact, and the DOM order follows the reader's requested ordering.
    for (const card of result.posts) list.append(card.node);
    search.value = state.query;
    keyword.value = state.keyword;
    sort.value = state.sort;
    for (const button of categories) button.setAttribute('aria-pressed', String(button.dataset.blogCategoryButton === state.category));
    for (const description of root.querySelectorAll('[data-blog-category-description]')) {
      description.hidden = description.dataset.blogCategoryDescription !== state.category;
    }
    const category = categories.find(button => button.dataset.blogCategoryButton === state.category).textContent;
    count.textContent = format(result.count === 1 ? root.dataset.result : root.dataset.results, { count: result.count, category });
    root.querySelector('[data-blog-empty]').hidden = result.count !== 0;
    root.querySelector('[data-blog-pagination]').hidden = result.pages < 2;
    root.querySelector('[data-blog-page-number]').textContent = format(root.dataset.pageLabel, { page: result.page, pages: result.pages });
    previous.disabled = result.page <= 1;
    next.disabled = result.page >= result.pages;
    reset.disabled = !state.query && !state.keyword && state.sort === 'newest' && state.page === 1;
    if (write) writeUrl();
  };
  const filter = () => {
    state = { ...state, query: search.value, keyword: keyword.value, sort: sort.value, page: 1 };
    render();
  };
  search.addEventListener('input', filter);
  keyword.addEventListener('change', filter);
  sort.addEventListener('change', filter);
  root.querySelector('form').addEventListener('submit', event => { event.preventDefault(); filter(); });
  for (const button of categories) button.addEventListener('click', () => {
    state.category = button.dataset.blogCategoryButton;
    state.page = 1;
    saveCategory();
    render();
  });
  reset.addEventListener('click', () => {
    state = { category: state.category, query: '', keyword: '', sort: 'newest', page: 1 };
    render();
    search.focus();
  });
  const turnPage = increment => {
    state.page += increment;
    render();
    count.tabIndex = -1;
    count.focus({ preventScroll: true });
    count.scrollIntoView({ block: 'start', behavior: 'instant' });
  };
  previous.addEventListener('click', () => turnPage(-1));
  next.addEventListener('click', () => turnPage(1));
  window.addEventListener('popstate', () => { state = readState(); render(false); });
  window.addEventListener('storage', event => {
    if (event.key !== BLOG_CATEGORY_KEY && event.key !== null) return;
    state.category = readCategory();
    state.page = 1;
    render();
  });
  // An explicit category link also becomes the preference for future visits.
  if (new URL(location.href).searchParams.has('category')) saveCategory();
  render(false);
  root.querySelector('[data-blog-controls]').hidden = false;
  root.dataset.blogReady = 'true';
}
