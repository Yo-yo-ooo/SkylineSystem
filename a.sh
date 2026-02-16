find ./kernel/src/arch/x86_64 -type f \( -name "*.c" -o -name "*.cpp" \) | while read -r file; do
    filename=$(basename "$file")
    # 创建一个临时文件，将模板中的 %FILENAME% 替换为实际文件名
    sed "s/%FILENAME%/$filename/g" a.txt > header.tmp
    # 将原来的代码内容追加到临时文件后面
    cat "$file" >> header.tmp
    # 覆盖原文件
    mv header.tmp "$file"
done