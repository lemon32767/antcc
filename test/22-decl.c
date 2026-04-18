/* EXPECT:
*/

void f(int (*)());
void f(int (*)(int));
void f(int (*)());
int a[];
int a[100];
int a[];

int main() {
}

