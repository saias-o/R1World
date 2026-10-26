const fs = require('fs');
const path = require('path');
const validator = require('./deps/node_modules/gltf-validator');
const root = path.resolve(__dirname, '../../game/assets/animals');
(async () => {
  const reports = [];
  for (const name of fs.readdirSync(root).filter(n => n.endsWith('.glb')).sort()) {
    const r = await validator.validateBytes(new Uint8Array(fs.readFileSync(path.join(root, name))), {uri: name, maxIssues: 30});
    reports.push({file: name, errors: r.issues.numErrors, warnings: r.issues.numWarnings, infos: r.issues.numInfos, messages: r.issues.messages});
  }
  fs.writeFileSync(path.join(__dirname, 'review/gltf-validation.json'), JSON.stringify(reports, null, 2));
  console.log(JSON.stringify({files: reports.length, errors: reports.reduce((a,r) => a+r.errors,0), warnings: reports.reduce((a,r) => a+r.warnings,0), failures: reports.filter(r=>r.errors).map(r=>({file:r.file,messages:r.messages}))}, null, 2));
  if (reports.some(r=>r.errors)) process.exitCode = 1;
})();
