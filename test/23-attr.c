/* EXPECT:
*/

#if !__has_attribute(noreturn)
#error "no noreturn"
#elif __has_attribute(foobar)
#error "attribute foobar !?"
#endif

int main(){}
