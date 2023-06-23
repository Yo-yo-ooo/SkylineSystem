# 设置编译器和编译选项
CXX = g++
CXXFLAGS = -ffreestanding -fshort-wchar -mno-red-zone -fno-omit-frame-pointer -fno-exceptions -fpermissive -Wno-pmf-conversions

# 源文件和目标文件存放路径
SRCDIR = src
OBJDIR = obj

# 获取所有.cpp文件
SRCS = $(wildcard $(SRCDIR)/*.cpp)

# 将.cpp文件转换为.obj文件
OBJS = $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRCS))

# 设置目标文件名
TARGET = myprogram

# 编译规则
$(TARGET): $(OBJS)
    $(CXX) $(CXXFLAGS) $^ -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
    $(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR):
    mkdir $(OBJDIR)

clean:
    rm -rf $(OBJDIR);
    rm -f $(TARGET)