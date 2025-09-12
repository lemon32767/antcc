

float get_value(unsigned x)
{
   static const float values [] = {1.1f, 1.2f, 1.3f, 1.4f};
    return x < 4 ? values[x] : 0.0f;
}
