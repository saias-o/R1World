// This is deliberately ordinary DOM code: keep UI presentation independent
// from native geography, streaming and persisted game options.
function showSettingsTab(name) {
    for (const tab of document.querySelectorAll('.settings-tab')) {
        const selected = tab.getAttribute('data-tab') === name;
        tab.classList.toggle('active', selected);
        tab.setAttribute('aria-selected', selected ? 'true' : 'false');
    }
    for (const panel of document.querySelectorAll('.settings-panel')) {
        panel.classList.toggle('active', panel.id === 'settings-' + name);
    }
}
for (const tab of document.querySelectorAll('.settings-tab')) {
    tab.addEventListener('click', function () {
        showSettingsTab(tab.getAttribute('data-tab'));
    });
}
document.getElementById('options').addEventListener('click', function () {
    document.getElementById('options-back').focus();
});
document.getElementById('options-back').addEventListener('click', function () {
    document.getElementById('options').focus();
});
document.getElementById('city-query').addEventListener('keydown', function (event) {
    if (event.key === 'Enter') {
        const first = document.getElementById('city-choice-0');
        if (first.getAttribute('data-available') === 'true') first.click();
        event.stopPropagation();
    }
    if (event.key === 'Escape') {
        document.getElementById('city-results').style.display = 'none';
        document.getElementById('city-query').blur();
        event.stopPropagation();
    }
});

const portfolioViewer = document.getElementById('portfolio-viewer');
const portfolioViewport = document.getElementById('portfolio-viewer-viewport');
const portfolioImage = document.getElementById('portfolio-viewer-image');
const portfolioView = { zoom: 1, x: 0, y: 0, width: 1320, height: 680, card: null, drag: null };

function renderPortfolioPhoto() {
    const viewportWidth = portfolioViewport.offsetWidth || 1440;
    const viewportHeight = portfolioViewport.offsetHeight || 710;
    const maxX = Math.max(0, (portfolioView.width * portfolioView.zoom - viewportWidth) / 2);
    const maxY = Math.max(0, (portfolioView.height * portfolioView.zoom - viewportHeight) / 2);
    portfolioView.x = Math.max(-maxX, Math.min(maxX, portfolioView.x));
    portfolioView.y = Math.max(-maxY, Math.min(maxY, portfolioView.y));
    portfolioImage.style.transform = 'translate(' + portfolioView.x + 'px, ' + portfolioView.y + 'px) scale(' + portfolioView.zoom + ')';
    portfolioViewport.style.cursor = portfolioView.zoom > 1 ? 'move' : 'default';
    portfolioViewer.setAttribute('data-zoom', String(portfolioView.zoom));
    portfolioViewer.setAttribute('data-pan-x', String(portfolioView.x));
    portfolioViewer.setAttribute('data-pan-y', String(portfolioView.y));
    document.getElementById('portfolio-zoom-reset').textContent = Math.round(portfolioView.zoom * 100) + ' %';
}

function zoomPortfolioPhoto(multiplier) {
    if (!portfolioViewer.classList.contains('open') || !Number.isFinite(multiplier) || multiplier <= 0) return;
    const previous = portfolioView.zoom;
    portfolioView.zoom = Math.max(1, Math.min(8, previous * multiplier));
    portfolioView.x *= portfolioView.zoom / previous;
    portfolioView.y *= portfolioView.zoom / previous;
    renderPortfolioPhoto();
}

function resetPortfolioPhoto() {
    portfolioView.zoom = 1;
    portfolioView.x = portfolioView.y = 0;
    portfolioView.drag = null;
    renderPortfolioPhoto();
}

function openPortfolioPhoto(card) {
    if (card.getAttribute('data-available') !== 'true') return;
    const slot = card.getAttribute('data-slot');
    const source = document.getElementById('portfolio-thumb-' + slot).getAttribute('src') || '';
    const ratio = Number(card.getAttribute('data-aspect')) || 1.6;
    portfolioView.card = card;
    portfolioView.width = Math.min(1320, 680 * ratio);
    portfolioView.height = portfolioView.width / ratio;
    portfolioImage.style.width = portfolioView.width + 'px';
    portfolioImage.style.height = portfolioView.height + 'px';
    portfolioImage.style.display = source ? 'block' : 'none';
    if (source) portfolioImage.setAttribute('src', source);
    else portfolioImage.removeAttribute('src');
    document.getElementById('portfolio-viewer-title').textContent = card.getAttribute('data-title') || '';
    document.getElementById('portfolio-viewer-details').textContent = card.getAttribute('data-details') || '';
    document.getElementById('portfolio-viewer-error').style.display = source ? 'none' : 'block';
    document.getElementById('player-card').style.visibility = 'hidden';
    portfolioViewer.classList.add('open');
    resetPortfolioPhoto();
    document.getElementById('portfolio-viewer-close').focus();
}

function closePortfolioViewer() {
    const wasOpen = portfolioViewer.classList.contains('open');
    portfolioViewer.classList.remove('open');
    document.getElementById('player-card').style.visibility = 'visible';
    portfolioImage.removeAttribute('src');
    resetPortfolioPhoto();
    if (wasOpen && portfolioView.card) portfolioView.card.focus();
    portfolioView.card = null;
}

for (const card of document.querySelectorAll('.portfolio-card')) {
    card.addEventListener('click', function () { openPortfolioPhoto(card); });
}
document.getElementById('portfolio-viewer-close').addEventListener('click', closePortfolioViewer);
document.getElementById('portfolio-zoom-in').addEventListener('click', function () { zoomPortfolioPhoto(1.25); });
document.getElementById('portfolio-zoom-out').addEventListener('click', function () { zoomPortfolioPhoto(0.8); });
document.getElementById('portfolio-zoom-reset').addEventListener('click', resetPortfolioPhoto);
portfolioViewport.addEventListener('wheel', function (event) {
    const steps = event.deltaY / (event.deltaMode === 0 ? 100 : 1);
    zoomPortfolioPhoto(Math.exp(-Math.max(-20, Math.min(20, steps)) * 0.18));
    event.stopPropagation();
});
portfolioViewport.addEventListener('mousedown', function (event) {
    if (event.button !== 0 || portfolioView.zoom <= 1) return;
    portfolioView.drag = { mouseX: event.clientX, mouseY: event.clientY, x: portfolioView.x, y: portfolioView.y };
    event.stopPropagation();
});
portfolioViewer.addEventListener('mousemove', function (event) {
    if (!portfolioView.drag || !portfolioViewer.classList.contains('open')) return;
    portfolioView.x = portfolioView.drag.x + event.clientX - portfolioView.drag.mouseX;
    portfolioView.y = portfolioView.drag.y + event.clientY - portfolioView.drag.mouseY;
    renderPortfolioPhoto();
});
portfolioViewer.addEventListener('mouseup', function () { portfolioView.drag = null; });
