#include <vec4d.h>

Vector4D::Vector4D(float x, float y, float z, float w)
{
	vec[0] = x;
	vec[1] = y;
	vec[2] = z;
	vec[3] = w;
}

Vector4D::Vector4D(const Vector3D& v, float w)
{
	vec[0] = v.x();
	vec[1] = v.y();
	vec[2] = v.z();
	vec[3] = w;
}

Vector4D& Vector4D::operator+=(const Vector4D& vector)
{
	setX(x() + vector.x());
	setY(y() + vector.y());
	setZ(z() + vector.z());
	setW(w() + vector.w());
	return *this;
}

Vector4D& Vector4D::operator-=(const Vector4D& vector)
{
	setX(x() - vector.x());
	setY(y() - vector.y());
	setZ(z() - vector.z());
	setW(w() - vector.w());
	return *this;
}

Vector4D& Vector4D::operator*=(const Vector4D& vector)
{
	setX(x() * vector.x());
	setY(y() * vector.y());
	setZ(z() * vector.z());
	setW(w() * vector.w());
	return *this;
}

Vector4D& Vector4D::operator/=(const Vector4D& vector)
{
	setX(x() / vector.x());
	setY(y() / vector.y());
	setZ(z() / vector.z());
	setW(w() / vector.w());
	return *this;
}

Vector4D& Vector4D::operator*=(float m)
{
	setX(x() * m);
	setY(y() * m);
	setZ(z() * m);
	setW(w() * m);
	return *this;
}

Vector4D& Vector4D::operator/=(float d)
{
	setX(x() / d);
	setY(y() / d);
	setZ(z() / d);
	setW(w() / d);
	return *this;
}

const Vector4D operator+(const Vector4D& v1, const Vector4D& v2)
{
	Vector4D v(v1);
	v += v2;
	return v;
}

const Vector4D operator-(const Vector4D& v1, const Vector4D& v2)
{
	Vector4D v(v1);
	v -= v2;
	return v;
}

const Vector4D operator*(const Vector4D& v1, const Vector4D& v2)
{
	Vector4D v(v1);
	v *= v2;
	return v;
}

const Vector4D operator/(const Vector4D& v1, const Vector4D& v2)
{
	Vector4D v(v1);
	v /= v2;
	return v;
}

const Vector4D operator-(const Vector4D& vector)
{
	Vector4D v;
	v -= vector;
	return v;
}

const Vector4D operator*(const Vector4D& v1, float m)
{
	Vector4D v(v1);
	v *= m;
	return v;
}

const Vector4D operator/(const Vector4D& v1, float d)
{
	Vector4D v(v1);
	v /= d;
	return v;
}