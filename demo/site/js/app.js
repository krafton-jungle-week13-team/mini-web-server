// 오늘의 메뉴: /api/menu.txt 는 프록시가 backend1 / backend2 로 번갈아 전달한다
const today = document.getElementById('today');
const reload = document.getElementById('reload');

async function loadToday() {
  today.innerHTML = '<img src="/img/loading.gif" alt="" width="32" height="32"> 불러오는 중…';
  try {
    // no-store: 브라우저 캐시 없이 매번 서버에 요청 (쿼리 스트링은 tiny가 정적 경로로 처리하지 않으므로 쓰지 않음)
    const res = await fetch('/api/menu.txt', { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const lines = (await res.text()).trim().split('\n');
    const served = lines.find(l => l.startsWith('served-by:'));
    const items = lines.filter(l => l && !l.startsWith('served-by:') && !l.startsWith('#'));

    today.innerHTML = '';
    const ul = document.createElement('ul');
    for (const item of items) {
      const li = document.createElement('li');
      li.textContent = item;
      ul.appendChild(li);
    }
    today.appendChild(ul);
    if (served) {
      const badge = document.createElement('span');
      badge.className = 'badge';
      badge.textContent = served.replace('served-by:', '').trim();
      today.appendChild(badge);
    }
  } catch (e) {
    today.textContent = `오늘의 메뉴를 불러오지 못했어요 (${e.message}). 백엔드 서버가 켜져 있는지 확인하세요.`;
  }
}

reload.addEventListener('click', loadToday);
loadToday();
