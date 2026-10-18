// Prueba la web de Cloud3DS en un móvil y un PC simulados. Uso: node web.js URL_DEL_QR salida/
const { chromium } = require('/opt/node22/lib/node_modules/playwright');
const fs = require('fs'), path = require('path');
const url = process.argv[2], out = process.argv[3];
const base = url.split('#')[0];
const mark = n => fs.writeFileSync(path.join(out, 'm_' + n), '');
const sleep = ms => new Promise(r => setTimeout(r, ms));
let fails = 0;
const check = (n, c, x = '') => { console.log((c ? 'OK   ' : 'FALLA') + ' ' + n + ' ' + x); if (!c) fails++; };

(async () => {
  const browser = await chromium.launch();
  const lang = process.env.WEB_LANG || 'es-ES';
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true, locale: lang,
    userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1',
    colorScheme: process.env.WEB_LIGHT ? 'light' : 'dark' });
  const page = await ctx.newPage();
  page.on('pageerror', e => { console.log('ERROR JS:', e.message); fails++; });
  const shot = async n => { await sleep(350); await page.screenshot({ path: path.join(out, n + '.png') }); };

  // 1) sin clave: pide PIN
  await page.goto(base);
  await page.waitForSelector('#pin input');
  await shot('w01_pin');
  await page.fill('#pin input >> nth=0', '0');
  for (const d of ['0', '0', '0']) await page.keyboard.type(d);
  await page.waitForSelector('.lmsg.err');
  await shot('w02_pin_bad');
  check('PIN incorrecto muestra error', await page.isVisible('.lmsg.err'));

  // 2) con el QR entra solo (misma página abierta: solo cambia el #)
  await page.goto(url);
  await page.waitForSelector('#list .row', { timeout: 20000 });
  check('el QR entra directamente', true);
  check('la URL ya no lleva la clave', !page.url().includes('k='), page.url());
  await shot('w03_root');
  mark('root');

  // 3) accesos rápidos
  await page.click('.chip:has-text("Capturas")');
  await page.waitForSelector('.row:has-text("_top.png")');
  await shot('w04_screens_list');
  await page.click('#bview');
  await page.waitForSelector('.tile img');
  await sleep(1500);
  await shot('w05_screens_grid');
  await page.click('.tile >> nth=0');
  await page.waitForSelector('.viewer img');
  await sleep(800);
  await shot('w06_viewer');
  await page.click('[data-v="n"]');
  await sleep(500);
  check('el visor avanza', (await page.textContent('.vcount')).startsWith('2'));
  await page.click('[data-v="x"]');
  await page.click('#bview');

  // 4) carpeta que no existe aún
  await page.click('.chip:has-text("Partidas")');
  await page.waitForSelector('#list .row, .empty');
  await page.click('.chip:has-text("CIAs")');
  await page.waitForSelector('.row:has-text(".cia")');
  await page.click('.row:has-text("Theme Shop") .more');
  await page.waitForSelector('.sheet');
  await shot('w07_sheet');
  await page.click('.act[data-a="ren"]');
  await page.waitForSelector('.dlg .field');
  await page.fill('.dlg .field', 'Theme Pack.cia');
  await shot('w08_rename');
  await page.click('.dlg [data-r="1"]');
  await page.waitForSelector('.row:has-text("Theme Pack.cia")');
  check('renombrar desde la web', true);

  // 5) subir un archivo grande y mirar la cola
  const big = path.join(out, 'video 4K vacaciones.mp4');
  fs.writeFileSync(big, Buffer.alloc(24 * 1024 * 1024, 7));
  const small = path.join(out, 'partida.sav');
  fs.writeFileSync(small, Buffer.from('hola 3ds'));
  await page.setInputFiles('#fpick', [big, small]);
  await page.waitForSelector('.queue.show .pbar');
  mark('upload');
  await sleep(250);
  await shot('w09_uploading');
  await page.waitForSelector('.qh:has-text("terminadas")', { timeout: 120000 });
  await shot('w10_uploaded');
  check('subidas terminadas', true);

  // 6) conflicto: volver a subir el mismo
  await page.setInputFiles('#fpick', [small]);
  await page.waitForSelector('.dlg:has-text("Ya existe")');
  await shot('w11_conflict');
  await page.click('[data-c="keep"]');
  await page.waitForSelector('.row:has-text("partida (2).sav")', { timeout: 20000 });
  check('conservar ambos crea «partida (2).sav»', true);
  await page.click('[data-qclose]').catch(() => {});

  // 7) selección y borrado
  await page.click('.row:has-text("partida (2).sav") .more');
  await page.click('.act[data-a="sel"]');
  await page.click('.row:has-text("partida.sav")');
  await shot('w12_selected');
  await page.click('#sDel');
  await page.waitForSelector('.dlg');
  await shot('w13_delete');
  await page.click('.dlg [data-r="1"]');
  await page.waitForFunction(() => !document.querySelector('.row[data-n="partida.sav"]'), null, { timeout: 20000 });
  check('borrado múltiple', true);

  // 8) carpeta del sistema pide confirmación
  await page.click('.chip:has-text("Inicio")');
  await page.waitForSelector('.row:has-text("luma")');
  await page.click('.row:has-text("boot.firm") .more');
  await page.click('.act[data-a="del"]');
  await page.click('.dlg [data-r="1"]');
  await page.waitForSelector('.dlg:has-text("sistema")');
  await shot('w14_system');
  await page.click('.dlg [data-r="0"]');
  await sleep(400);
  check('boot.firm sigue ahí', await page.isVisible('.row:has-text("boot.firm")'));

  // 9) texto
  await page.click('.row:has-text("notes.txt")');
  await page.waitForSelector('.viewer pre:not(:has(.spin))');
  await shot('w15_text');
  await page.click('[data-v="x"]');

  // 10) PC
  const pc = await browser.newContext({ viewport: { width: 1280, height: 800 }, locale: lang, colorScheme: process.env.WEB_LIGHT ? 'light' : 'dark' });
  const p2 = await pc.newPage();
  p2.on('pageerror', e => { console.log('ERROR JS (pc):', e.message); fails++; });
  await p2.goto(url);
  await p2.waitForSelector('#list .row');
  await p2.click('.chip:has-text("Capturas")');
  await p2.waitForSelector('.row:has-text("_top.png")');
  await p2.click('#bview');
  await p2.waitForSelector('.tile img');
  await sleep(1500);
  await p2.screenshot({ path: path.join(out, 'w16_pc_grid.png') });
  await p2.click('#bview');
  mark('done');
  await browser.close();
  console.log('FALLOS:', fails);
  process.exit(fails ? 1 : 0);
})().catch(e => { console.log('EXCEPCIÓN:', e.message); mark('done'); process.exit(2); });
