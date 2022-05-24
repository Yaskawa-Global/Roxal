#!/bin/bash
java -Xmx500M -cp "/opt/antlr/antlr-4.9.2-complete.jar:$CLASSPATH" org.antlr.v4.Tool -Dlanguage=Cpp -package "roxal" -no-listener -visitor ../core/Roxal.g4 -o cpp-gen

