const form = document.querySelector('form');
const result = document.querySelector('#result');
const button = document.querySelector('button');
const ruleFields = [...form.querySelectorAll('[data-fuzzy]')];
const fuzzyRules = form.querySelector('#fuzzy-rules');
let native;

function render(response) {
  native = response.native;
  for (const [name, value] of Object.entries({ ...native, ...response.display })) {
    const field = form.elements.namedItem(name);
    if (!field) continue;
    if (field.type === 'checkbox') field.checked = value;
    else field.value = value;
  }
  for (const field of ruleFields) field.checked = (native.FuzzyPairs & Number(field.dataset.fuzzy)) !== 0;
  fuzzyRules.disabled = !native.FuzzyEnabled;
  document.querySelector('#cloud-state').textContent = response.cloudEnabled ? '已启用' : '尚未启用';
}

async function settingsRequest(options) {
  const response = await fetch('/api/settings', options);
  const value = await response.json();
  if (!response.ok) throw new Error(value.error);
  return value;
}

form.elements.FuzzyEnabled.addEventListener('change', () => {
  fuzzyRules.disabled = !form.elements.FuzzyEnabled.checked;
});

form.addEventListener('submit', async (event) => {
  event.preventDefault();
  button.disabled = true;
  result.textContent = '正在保存…';
  let fuzzyPairs = native.FuzzyPairs;
  for (const { checked, dataset } of ruleFields) {
    const mask = Number(dataset.fuzzy);
    fuzzyPairs = checked ? fuzzyPairs | mask : fuzzyPairs & ~mask;
  }

  try {
    const response = await settingsRequest({
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        native: {
          AutoCorrection: form.elements.AutoCorrection.checked,
          FuzzyEnabled: form.elements.FuzzyEnabled.checked,
          FuzzyPairs: fuzzyPairs >>> 0,
          SelfLearning: form.elements.SelfLearning.checked,
        },
        display: {
          PageSize: Number(form.elements.PageSize.value),
          ShiftSwitch: form.elements.ShiftSwitch.checked,
          ChinesePunctuation: form.elements.ChinesePunctuation.checked,
          FullWidth: form.elements.FullWidth.checked,
        },
      }),
    });
    render(response);
    result.textContent = response.applied ? '设置已应用。' : '引擎已加载新设置。显示选项将在 Fcitx5 下次启动时生效。';
  } catch (error) {
    result.textContent = error.message;
  } finally {
    button.disabled = false;
  }
});

try {
  render(await settingsRequest());
  button.disabled = false;
} catch (error) {
  result.textContent = error.message;
}
