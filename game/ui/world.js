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
