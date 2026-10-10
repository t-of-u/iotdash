// Sidebar, "on this page" list and previous/next links for every documentation page.
// One list here: adding a page means adding it to NAV (paths are relative to docs/).
(function () {
  const NAV = [
    ['เริ่มต้น', [
      ['index.html', 'ภาพรวม iotdash'],
      ['guide/getting-started.html', 'ติดตั้งและใช้งานครั้งแรก'],
    ]],
    ['แนวคิดหลัก', [
      ['concepts/architecture.html', 'สถาปัตยกรรมระบบ'],
      ['concepts/os.html', 'iotdash OS'],
    ]],
    ['แกนระบบ (core)', [
      ['core/hardware.html', 'ฮาร์ดแวร์และ flash'],
      ['core/display.html', 'การแสดงผล (scan-out)'],
      ['core/memory-storage.html', 'หน่วยความจำและ storage'],
      ['core/engine.html', 'Rendering engine'],
      ['core/js-runtime.html', 'JavaScript runtime'],
      ['core/input.html', 'Touch และ input'],
      ['core/connectivity.html', 'Wi-Fi และ Bluetooth'],
    ]],
    ['คู่มือ', [
      ['guide/app-development.html', 'เขียนแอปสำหรับ iotdash'],
      ['guide/store.html', 'App store (iotstore)'],
    ]],
    ['อ้างอิง', [
      ['reference/js-api.html', 'JavaScript API ของ OS'],
      ['reference/cli.html', 'เครื่องมือ command line'],
      ['reference/devlink.html', 'Dev link และคำสั่งบนบอร์ด'],
      ['reference/file-formats.html', 'Format ไฟล์และ layout'],
    ]],
    ['เครื่องมือและการ debug', [
      ['tools/simulator.html', 'Simulator'],
      ['tools/debugging.html', 'การวัดผลและ debug'],
    ]],
    ['โปรเจกต์', [
      ['project/changelog.html', 'ประวัติการพัฒนา'],
      ['project/known-issues.html', 'ข้อจำกัดและงานที่ค้าง'],
    ]],
  ];

  const script = document.currentScript || document.querySelector('script[src$="nav.js"]');
  const root = new URL('..', script.src);           // docs/
  const here = location.href.split('#')[0];
  const flat = [];
  NAV.forEach(([, items]) => items.forEach(([href, title]) => flat.push({href, title, url: new URL(href, root).href})));

  // sidebar
  const side = document.getElementById('sidebar');
  if (side) {
    let html = `<a class="brand" href="${new URL('index.html', root).href}"><span class="logo">i</span>iotdash docs</a>`;
    NAV.forEach(([group, items]) => {
      html += `<div class="group">${group}</div>`;
      items.forEach(([href, title]) => {
        const url = new URL(href, root).href;
        html += `<a class="item${url === here ? ' on' : ''}" href="${url}">${title}</a>`;
      });
    });
    side.innerHTML = html;
  }

  const main = document.querySelector('main');
  if (!main) return;

  // on this page: the h2 headings
  const h2s = [...main.querySelectorAll('h2')];
  const slot = document.getElementById('toc');
  if (slot && h2s.length > 2) {
    h2s.forEach((h, i) => { if (!h.id) h.id = 's' + (i + 1); });
    slot.className = 'toc';
    slot.innerHTML = '<b>ในหน้านี้</b><ol>' + h2s.map((h) => `<li><a href="#${h.id}">${h.textContent}</a></li>`).join('') + '</ol>';
  }

  // previous / next
  const i = flat.findIndex((p) => p.url === here);
  if (i >= 0) {
    const prev = flat[i - 1], next = flat[i + 1];
    const pager = document.createElement('div');
    pager.className = 'pager';
    if (prev) pager.innerHTML += `<a href="${prev.url}"><span class="lbl">← ก่อนหน้า</span>${prev.title}</a>`;
    if (next) pager.innerHTML += `<a class="next" href="${next.url}"><span class="lbl">ถัดไป →</span>${next.title}</a>`;
    main.appendChild(pager);
  }
})();
