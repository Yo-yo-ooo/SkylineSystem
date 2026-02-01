#ifndef PAIR_H_
#define PAIR_H_
namespace EasySTL {
template<class T1, class T2>
struct pair {
	typedef T1 first_type;
	typedef T2 second_type;

	T1 first;
	T2 second;
	pair() : first(T1()), second(T2()) {}
	pair(const T1& a, const T2& b) : first(a), second(b) {}
};
template<class T1, class T2>
bool operator==(const pair<T1, T2>& s1, const pair<T1, T2>& s2)
{
    return s1.first == s2.first && s1.second == s2.second;
}

template<class T1, class T2>
bool operator>(const pair<T1, T2>& s1, const pair<T1, T2>& s2)
{
    return (s1.first > s2.first) || (!(s1.first < s2.first) && s1.second > s2.second);
}

template<class T1, class T2>
bool operator<(const pair<T1, T2>& s1, const pair<T1, T2>& s2)
{
    return (s1.first < s2.first) || (!(s1.first > s2.first) && s1.second < s2.second);
}

template <typename T1, typename T2>
pair<T1, T2> make_pair(T1 x, T2 y) {
    return pair<T1, T2>(x, y);
}
}
#endif