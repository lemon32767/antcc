/* EXPECT:
6! = 720
*/

int
fact(int x)
{
   int y = 1;
   while (x >= 1) {
      y *= x;
      x -= 1;
   }
   return y;
}

extern int printf();
int main() { printf("6! = %d\n", fact(6)); }
