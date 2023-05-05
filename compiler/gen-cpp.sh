#!/bin/bash
java -Xmx500M -cp "/opt/antlr/antlr-4.12.0-complete.jar:$CLASSPATH" org.antlr.v4.Tool -Dlanguage=Cpp -package "roxal" -no-listener -visitor Roxal.g4 -o cpp-gen

