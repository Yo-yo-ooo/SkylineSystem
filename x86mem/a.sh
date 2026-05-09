git diff HEAD -- memset.c | awk '
/^[+-][^+-]/ { 
    total++
    if ($0 ~ /static/) static_count++ 
} 
END { 
    if (total > 0) 
        printf "包含 static 的修改行数: %d\n总修改行数: %d\nstatic 影响行占比: %.2f%%\n", static_count, total, (static_count/total)*10000 
    else 
        print "该文件没有修改" 
}'