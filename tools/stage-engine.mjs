import { copyFileSync, cpSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const [, , prefix, packageRoot, tableRoot, registryFile] = process.argv;
if (!registryFile) throw new Error('Expected Wine prefix, MSI extraction, table directory, and registry output');
const destination = join(prefix, 'drive_c', 'Program Files', 'BingKeyboard');
const components = new Set([
  'ProductRegistry', 'MAI_MTF', 'MAI_MTFServer', 'MtfServerPS',
  'MAI_ChsPinyinDS', 'MAI_ChsAdvancedDS', 'MAI_ChsLexiconUpdateDS',
  'MAI_ServiceDS', 'MAI_ChsProxyDS',
]);

function table(name) {
  return readFileSync(join(tableRoot, `${name}.tsv`), 'utf8').trimEnd()
    .split(/\r?\n/).slice(3).map((line) => line.split('\t'));
}

const install = 'C:\\Program Files\\BingKeyboard\\';
const properties = {
  PRODUCT_FOLDER: install,
  DICTS_CHS_FOLDER: `${install}Dicts\\CHS\\`,
};
const files = new Map();
mkdirSync(join(destination, 'Server'), { recursive: true });
for (const [id, component, name] of table('File')) {
  if (!components.has(component) || component === 'ProductRegistry') continue;
  const filename = name.split('|').at(-1);
  files.set(`#${id}`, `${install}Server\\${filename}`);
  copyFileSync(join(packageRoot, 'Server', filename), join(destination, 'Server', filename));
}
cpSync(join(packageRoot, 'Dicts'), join(destination, 'Dicts'), { recursive: true });

const keys = new Map();
const quote = (value) => `"${value.replaceAll('\\', '\\\\').replaceAll('"', '\\"')}"`;
for (const [, root, key, name, value, component] of table('Registry')) {
  if (!components.has(component)) continue;
  if (name === 'CurrentSkinFileName') continue;
  const hive = { 0: 'HKEY_CLASSES_ROOT', 2: 'HKEY_LOCAL_MACHINE' }[root];
  if (!hive) throw new Error(`Unexpected registry root ${root}`);
  const fullKey = `${hive}\\${key}`;
  const expanded = value.replace(/\[([^\]]+)\]/g, (_, property) => {
    const text = properties[property] ?? files.get(property);
    if (text === undefined) throw new Error(`Unknown MSI property ${property}`);
    return text;
  });
  if (!keys.has(fullKey)) keys.set(fullKey, []);
  keys.get(fullKey).push(`${name ? quote(name) : '@'}=${quote(expanded)}`);
}

const lines = ['Windows Registry Editor Version 5.00', ''];
for (const [key, values] of keys) lines.push(`[${key}]`, ...values, '');
writeFileSync(registryFile, `\ufeff${lines.join('\r\n')}`, 'utf16le');
console.log(`Staged ${files.size} engine DLLs and ${keys.size} registry keys`);
