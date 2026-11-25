const $ = id => document.getElementById(id);

async function timed(url, opts = {}) {
  const t0 = performance.now();
  try {
    const res = await fetch(url, { cache: 'no-store', ...opts });
    await res.arrayBuffer();
    return { status: res.status, ms: Math.round(performance.now() - t0) };
  } catch (e) {
    return { status: `실패 (${e.message})`, ms: Math.round(performance.now() - t0) };
  }
}

// 1. 느린 CGI와 정적 파일을 동시에 요청
$('block-test').addEventListener('click', async () => {
  const out = $('block-out');
  out.textContent = '측정 중… (약 3초)';
  const slow = timed('/cgi-bin/slow?sec=3');
  await new Promise(r => setTimeout(r, 200));      // slow가 먼저 서버에 도착하도록
  const home = timed('/home.html');
  const [s, h] = await Promise.all([slow, home]);
  out.textContent =
    `slow?sec=3   → ${s.status}, ${s.ms}ms\n` +
    `home.html    → ${h.status}, ${h.ms}ms  (0.2초 뒤에 요청)\n\n` +
    (h.ms > 1500
      ? '작은 정적 파일인데도 CGI가 끝날 때까지 기다렸어요. 이벤트 루프가 블로킹된 거예요.'
      : 'home.html이 바로 응답했어요. 블로킹이 해결된 서버예요!');
});

// 4. POST 관련 에러
$('err-test').addEventListener('click', async () => {
  const out = $('err-out');
  out.textContent = '요청 중…';
  const big = 'msg=' + 'a'.repeat(9000);
  const cases = [
    ['POST /home.html (정적 파일에 POST)', '/home.html', { method: 'POST', body: 'a=1' }, 405],
    ['POST /cgi-bin/guestbook (body 9KB)', '/cgi-bin/guestbook', { method: 'POST', body: big }, 413],
    ['PUT /home.html', '/home.html', { method: 'PUT', body: 'a=1' }, 501],
  ];
  const lines = [];
  for (const [label, url, opts, want] of cases) {
    const r = await timed(url, opts);
    lines.push(`${r.status === want ? 'OK ' : '?? '} ${label} → ${r.status} (기대 ${want})`);
  }
  out.textContent = lines.join('\n');
});

// 5. 라운드로빈
$('rr-test').addEventListener('click', async () => {
  const out = $('rr-out');
  out.textContent = '';
  for (let i = 1; i <= 6; i++) {
    try {
      const res = await fetch('/api/menu.txt', { cache: 'no-store' });
      const text = await res.text();
      const served = (text.match(/served-by:\s*(.+)/) || [])[1] || '-';
      out.textContent += `${i}번째 → ${res.status} ${served}\n`;
    } catch (e) {
      out.textContent += `${i}번째 → 실패 (${e.message})\n`;
    }
  }
});
