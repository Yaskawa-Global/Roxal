#!/usr/bin/env python3
"""Refresh an AI Studio model catalog from the ONNX files beside it.

    tools/nn-catalog.py examples/aistudio/models

Reads <dir>/catalog.json (hand-written fields: label, license, url, blurb,
pre, post -- what the palette and the editor's properties panel show, and
which vision helpers pair with the model; a blurb is seeded from the file's
own doc_string or metadata when it has one and the catalog does not) and rewrites it with each model's inputs and outputs (name, dtype,
shape; symbolic dims kept as strings) read from the .onnx file, so the
editor can type a model node's ports without ever loading the weights.
Models present on disk but absent from the catalog are added with a bare
entry; catalog entries whose file is missing are kept but flagged.
"""
import json
import os
import sys

import onnx

DTYPE = {
    1: 'float32', 2: 'uint8', 3: 'int8', 4: 'uint16', 5: 'int16', 6: 'int32',
    7: 'int64', 9: 'bool', 10: 'float16', 11: 'float64',
}


def tensor_spec(vi):
    t = vi.type.tensor_type
    dims = []
    for d in t.shape.dim:
        if d.HasField('dim_value'):
            dims.append(d.dim_value)
        else:
            dims.append(d.dim_param or '?')
    return {'name': vi.name, 'dtype': DTYPE.get(t.elem_type, str(t.elem_type)), 'shape': dims}


def describe(path):
    m = onnx.load(path, load_external_data=False)
    inits = {i.name for i in m.graph.initializer}
    inputs = [tensor_spec(v) for v in m.graph.input if v.name not in inits]
    outputs = [tensor_spec(v) for v in m.graph.output]
    opset = max((o.version for o in m.opset_import if o.domain in ('', 'ai.onnx')), default=None)
    return inputs, outputs, opset, self_description(m)


def self_description(m):
    """What the file says about itself, if anything.

    ONNX has a doc_string on the model and on the graph, plus free-form
    metadata_props. Nothing the usual exporters produce fills them in --
    torch.onnx.export writes the producer name and stops -- so this is a
    seed for a missing blurb, never a replacement for a written one.
    """
    for text in (m.doc_string, m.graph.doc_string):
        if text and text.strip():
            return ' '.join(text.split())
    for key in ('description', 'doc', 'model_description', 'summary'):
        for p in m.metadata_props:
            if p.key.lower() == key and p.value.strip():
                return ' '.join(p.value.split())
    return ''


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    mdir = argv[1]
    cat_path = os.path.join(mdir, 'catalog.json')
    catalog = {'models': []}
    if os.path.exists(cat_path):
        with open(cat_path) as f:
            catalog = json.load(f)
    by_file = {m['file']: m for m in catalog['models']}
    for fn in sorted(os.listdir(mdir)):
        if fn.endswith('.onnx') and fn not in by_file:
            entry = {'name': os.path.splitext(fn)[0], 'file': fn}
            catalog['models'].append(entry)
            by_file[fn] = entry
    for m in catalog['models']:
        path = os.path.join(mdir, m['file'])
        if not os.path.exists(path):
            m['missing'] = True
            print(f"  {m['file']}: MISSING (run download-models.sh)")
            continue
        m.pop('missing', None)
        inputs, outputs, opset, blurb = describe(path)
        m['inputs'] = inputs
        m['outputs'] = outputs
        m['opset'] = opset
        m['bytes'] = os.path.getsize(path)
        if blurb and not m.get('blurb'):
            m['blurb'] = blurb        # the file described itself; a written one wins
        print(f"  {m['file']}: {len(inputs)} in, {len(outputs)} out, opset {opset}, {m['bytes'] // 1024} KB")
    with open(cat_path, 'w') as f:
        json.dump(catalog, f, indent=2)
        f.write('\n')
    print(f"wrote {cat_path} ({len(catalog['models'])} models)")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
