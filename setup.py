from setuptools import find_packages, setup

setup(
    name="seastar",
    description='High performance server-side application framework',
    url='https://github.com/scylladb/seastar',
    download_url='https://github.com/scylladb/seastar/tags',
    license='AGPL',
    platforms='any',
    packages=find_packages(),
    include_package_data=True,
    install_requires=[])
