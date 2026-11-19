"""Translate Microsoft's fixed XML rows into compile-time test data; no expected values are recomputed."""
import json
import sys
import xml.etree.ElementTree as ET

rows = ET.parse(sys.argv[1]).findall("./Table[@Id='UnaryIntOpTable']/Row")
print("// Generated from the pinned Microsoft DXC UnaryIntOpTable.xml.")
print("struct DxcCase { const char *name; std::string (*compile)(const Compiler &); int32_t input[9], expected[9]; };")
print("static const DxcCase dxc_cases[] = {")
for row in rows:
    parameters = {p.attrib["Name"]: p for p in row.findall("Parameter")}
    assert parameters["ShaderOp.Target"].text == "cs_6_0"
    assert float(parameters["Validation.Tolerance"].text) == 0
    inputs = [int(v.text) for v in parameters["Validation.Input1"]]
    expected = [int(v.text) for v in parameters["Validation.Expected1"]]
    assert len(inputs) == len(expected) == 9
    assert all(-(2**31) <= n < 2**31 for n in inputs + expected)
    shader = parameters["ShaderOp.Text"].text
    assert ')DXC"' not in shader
    print("{" + json.dumps(row.attrib["Name"]) + ', [](const Compiler &compiler) {')
    print('const char source[] = R"DXC(' + shader + ')DXC";')
    print('return compiler.compile(source, "main", "cs_6_0"); },')
    print("{" + ",".join(map(str, inputs)) + "}, {" + ",".join(map(str, expected)) + "}},")
assert len(rows) == 4
print("};")
